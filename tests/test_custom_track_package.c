#include "custom_track_package.h"
#include "custom_track_prepared.h"
#include "custom_track_package_admission.h"
#include "custom_track_decode.h"
#include "mod_source_stream.h"
#include "fs_utf8.h"
#include "miniz.h"
#include "waves.h"
#include "f3ddkr.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#define process_id() ((long)_getpid())
#else
#include <unistd.h>
#define make_dir(p) mkdir(p, 0700)
#define remove_dir(p) rmdir(p)
#define process_id() ((long)getpid())
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "package line %d\n", __LINE__); return 1; } } while (0)
static unsigned allocations, fail_allocation, live_allocations;
void *mdkr_track_test_malloc(size_t size) {
    if (++allocations == fail_allocation) return NULL;
    void *p = malloc(size);
    if (p) ++live_allocations;
    return p;
}
void *mdkr_track_test_calloc(size_t count, size_t size) {
    if (++allocations == fail_allocation) return NULL;
    void *p = calloc(count, size);
    if (p) ++live_allocations;
    return p;
}
void mdkr_track_test_free(void *p) {
    if (p) {
        if (!live_allocations) abort();
        --live_allocations;
    }
    free(p);
}
void *mdkr_custom_track_test_malloc(size_t size) { return mdkr_track_test_malloc(size); }
void *mdkr_custom_track_test_calloc(size_t count, size_t size) { return mdkr_track_test_calloc(count, size); }
void mdkr_custom_track_test_free(void *p) { mdkr_track_test_free(p); }
static const char manifest[] = "{\"schemaVersion\":1,\"id\":\"lake\",\"adds\":["
    "{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"},"
    "{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"}]}";
typedef struct Fixture {
    unsigned calls, fail_call, mode, data_reads;
    const char *manifest_override;
    int invalid_vehicle;
    int invalid_texture_profile;
    unsigned wave_subdivisions;
    unsigned pulse_model; /* 1: package texture; 2: retail texture. */
    MdkrCustomTrackPayload pulse_texture;
    unsigned course_maps;
} Fixture;
typedef struct PulseFixture {
    unsigned calls;
    int refuse;
    unsigned char texture[64];
    MdkrCustomTrackPayload override;
} PulseFixture;
static int pulse_texture_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    PulseFixture *fixture = context;
    ++fixture->calls;
    if (fixture->refuse || id != 0) return 0;
    *out = fixture->override.bytes ? fixture->override :
        (MdkrCustomTrackPayload){fixture->texture, sizeof fixture->texture};
    return 1;
}
static int pulse_model_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    if (id != 0) return 0;
    *out = *(const MdkrCustomTrackPayload *)context;
    return 1;
}
typedef struct MiscFixture { unsigned calls, fail_call, short_call; } MiscFixture;
typedef struct ModelFixture {
    unsigned char bytes[10 + 0xC0];
    unsigned calls;
    int refuse;
} ModelFixture;
static int model_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    ModelFixture *fixture = context;
    ++fixture->calls;
    if (fixture->refuse || id != 0) return 0;
    out->bytes = fixture->bytes;
    out->size = sizeof fixture->bytes;
    return 1;
}
static int misc_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    static const unsigned char colour[24] = {0};
    static const unsigned char pulse[16] = {[1] = 1};
    MiscFixture *f = context;
    if (++f->calls == f->fail_call || id > 1) return 0;
    out->bytes = id ? pulse : colour;
    out->size = id ? sizeof pulse : sizeof colour;
    if (f->calls == f->short_call) --out->size;
    return 1;
}
static int fixture_read(void *ctx, const char *path, void *bytes, size_t cap, size_t *size) {
    Fixture *f = ctx;
    ++f->calls;
    *size = 0;
    if (f->calls == f->fail_call) return MDKR_MOD_SOURCE_IO_ERROR;
    const int metadata = !strcmp(path, "manifest.json");
    const char *data = metadata ? manifest : !strcmp(path, "header.bin") ? "HEAD" : "TEX";
    size_t n = strlen(data);
    unsigned char model_stream[10 + 0xE0] = {0};
    unsigned char density_header[200];
    static const char textured_manifest[] = "{\"schemaVersion\":1,\"id\":\"budget\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"},{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"},{\"section\":\"TEXTURES_3D\",\"file\":\"texture2.bin\"}]}";
    static const unsigned char texture[64] = {[0]=8, [1]=2, [2]=0x11, [0x12]=1, [0x17]=64};
    static const unsigned char bad_texture[64] = {0};
    if (f->mode >= 5) {
        if (metadata) { data = textured_manifest; n = strlen(data); }
        else if (strcmp(path, "header.bin")) {
            data = (const char *)(f->mode == 6 && !strcmp(path, "texture2.bin") ? bad_texture : texture);
            n = sizeof texture;
        }
    }
    if (f->mode >= 7) {
        static const char names_manifest[] = "{\"schemaVersion\":1,\"id\":\"names\",\"adds\":[{\"section\":\"LEVEL_NAMES\",\"file\":\"names.bin\"}]}";
        static const char names[] = "Lake\0Lac\0See\0Lago\0";
        data = metadata ? names_manifest : names;
        n = metadata ? strlen(names_manifest) : sizeof names - 1 - (f->mode == 8);
    }
    if (f->mode == 9 && metadata) {
        data = "{\"schemaVersion\":1,\"id\":\"layout\",\"adds\":["
            "{\"section\":\"LEVEL_OBJECT_MAPS\",\"slot\":\"collectables\",\"file\":\"a\"},"
            "{\"section\":\"TEXTURES_3D\",\"file\":\"b\"},"
            "{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"},"
            "{\"section\":\"LEVEL_OBJECT_MAPS\",\"slot\":\"structure\",\"file\":\"d\"},"
            "{\"section\":\"LEVEL_MODELS\",\"file\":\"e\"},"
            "{\"section\":\"TEXTURES_3D\",\"file\":\"f\"}]}";
        n = strlen(data);
    }
    if (f->mode == 10 && metadata) {
        data = "{\"schemaVersion\":1,\"id\":\"retained\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"}]}";
        n = strlen(data);
    }
    if (f->mode >= 9 && !metadata && !strcmp(path, "c")) {
        static const unsigned char good_header[MDKR_TRACK_HEADER_BYTES + 4] = {
            [0] = 6, [0x49] = 0xFF, [0x56] = 4, [0x57] = 8, [0x61] = 120,
            [0xAF] = 1, [MDKR_TRACK_HEADER_BYTES] = 0xA5};
        static const unsigned char bad_header[MDKR_TRACK_HEADER_BYTES + 4] = {
            [0] = 6, [0x35] = 55, [MDKR_TRACK_HEADER_BYTES] = 0xA5};
        data = (const char *)(f->mode == 10 ? bad_header : good_header);
        n = sizeof good_header;
        if (f->mode == 19 || f->mode == 20) {
            memcpy(density_header, good_header, sizeof density_header);
            if (f->mode == 19) density_header[0x71] = 1;
            else memset(density_header + 0x74, 0xFF, 4);
            data = (const char *)density_header;
        }
    }
    if (f->mode >= 11) {
        if (metadata) {
            data = "{\"schemaVersion\":1,\"id\":\"model\",\"adds\":["
                "{\"section\":\"LEVEL_MODELS\",\"file\":\"model.bin\"},"
                "{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"}]}";
            if (f->mode >= 14)
                data = "{\"schemaVersion\":1,\"id\":\"waves\",\"adds\":["
                    "{\"section\":\"LEVEL_MODELS\",\"file\":\"model.bin\"},"
                    "{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"},"
                    "{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"}]}";
            n = strlen(data);
        } else if (!strcmp(path, "model.bin")) {
            /* Independent stored block with a water basis and local texture.
             * No faces: admission composition, not playable geometry. */
            model_stream[0] = 0xC0; model_stream[4] = 9;
            model_stream[5] = 1; model_stream[6] = 0xC0;
            model_stream[8] = 0x3F; model_stream[9] = 0xFF;
            unsigned char *model = model_stream + 10;
            model[3] = 0xB8; model[7] = 0x4C; model[11] = 0x90;
            model[0x19] = 1; model[0x1B] = 1; model[0x4B] = 0xC0;
            model[0x5B] = 0x9C;
            model[0x6D] = 1; /* One batch plus sentinel. */
            model[0x97] = model[0x9B] = 100; /* Positive X/Z extents. */
            model[0xA4] = 1; model[0xA6] = 0x20; /* Water selection flags. */
            if (f->mode == 15) model[0x97] = 0; /* Zero-width basis. */
            if (f->mode == 16) model[0xA6] |= 1; /* Hidden: no candidate. */
            if (f->mode == 17) model[0x9C] = 0xFF; /* Missing texture. */
            if (f->mode == 18) model[0xA5] = 0x40; /* Height read, no vertices. */
            if (f->mode >= 20) model[0xA4] |= 0x10; /* Select colour slot 1. */
            model[0x74] = model[0x75] = 0xFF;
            model[0x77] = f->mode >= 13; /* hasWaves */
            model[0xBA] = 0x70; model[0xBB] = f->mode == 12 ? 1 : 0;
            data = (const char *)model_stream; n = 10 + 0xC0;
        }
    }
    if (f->mode >= 22) {
        if (metadata) {
            data = "{\"schemaVersion\":1,\"id\":\"maps\",\"adds\":["
                "{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"map.bin\",\"slot\":\"collectables\"}]}";
            if (f->mode >= 26)
                data = "{\"schemaVersion\":1,\"id\":\"maps\",\"adds\":["
                    "{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"map.bin\",\"slot\":\"collectables\"},"
                    "{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"second.bin\",\"slot\":\"structure\"}]}";
            n = strlen(data);
        } else {
            memset(model_stream, 0, sizeof model_stream);
            unsigned extent = f->mode >= 24 ? 24 : 16;
            model_stream[0] = extent; model_stream[4] = 9;
            model_stream[5] = 1; model_stream[6] = extent;
            model_stream[8] = 255 - extent; model_stream[9] = 255;
            if (f->mode == 23) model_stream[13] = 1; /* Missing declared record. */
            if (f->mode >= 24) {
                model_stream[13] = 8; model_stream[27] = 8;
                model_stream[26] = f->mode == 25 ? 1 : 0;
                if (f->mode == 27 && !strcmp(path, "second.bin")) model_stream[27] = 7;
                if (f->mode == 28 && !strcmp(path, "second.bin")) model_stream[0] = 25;
            }
            data = (const char *)model_stream; n = extent + 10;
        }
    }
    if (f->pulse_model && !metadata && !strcmp(path, "texture.bin")) {
        data = (const char *)texture; n = sizeof texture;
        if (f->pulse_texture.bytes) {
            data = f->pulse_texture.bytes; n = f->pulse_texture.size;
        }
    }
    if (f->pulse_model && !metadata && !strcmp(path, "model.bin")) {
        memset(model_stream, 0, sizeof model_stream);
        model_stream[0] = 0xE0; model_stream[4] = 9;
        model_stream[5] = 1; model_stream[6] = 0xE0;
        model_stream[8] = 0x1F; model_stream[9] = 0xFF;
        unsigned char *model = model_stream + 10;
        model[3] = 0xD8; model[7] = 0x4C; model[11] = 0x90;
        model[0x19] = model[0x1B] = 1; model[0x4B] = 0xE0;
        model[0x4F] = 0xB4; model[0x53] = 0xC0;
        model[0x5B] = 0x9C; model[0x63] = 0xD0;
        model[0x69] = model[0x6B] = model[0x6D] = 1;
        model[0x74] = model[0x75] = 0xFF;
        model[0xA5] = 4; /* Pulse material on one nonempty batch. */
        model[0xAB] = model[0xAD] = 1;
        model[0xDA] = f->pulse_model == 1 ? 0x70 : 0;
        data = (const char *)model_stream; n = sizeof model_stream;
    }
    if (f->course_maps && !metadata && !strcmp(path, "c")) {
        memset(density_header, 0, sizeof density_header);
        if (f->course_maps == 4) density_header[0xBB] = 137; /* Prior custom map. */
        data = (const char *)density_header; n = sizeof density_header;
    }
    if (metadata && f->manifest_override) {
        data = f->manifest_override;
        n = strlen(data);
    }
    if (!metadata && !strcmp(path, "c") && (f->invalid_vehicle || f->wave_subdivisions)) {
        if (n != sizeof density_header) return MDKR_MOD_SOURCE_IO_ERROR;
        if (data != (const char *)density_header) memcpy(density_header, data, n);
        if (f->invalid_vehicle) density_header[0x4D] = 255;
        if (f->wave_subdivisions) density_header[0x56] = f->wave_subdivisions;
        data = (const char *)density_header;
    }
    if (!metadata && f->invalid_texture_profile && !strcmp(path, "texture2.bin")) {
        if (n > sizeof model_stream) return MDKR_MOD_SOURCE_IO_ERROR;
        memcpy(model_stream, data, n);
        model_stream[0] = 7; /* Valid bytes, incompatible authored wrap profile. */
        data = (const char *)model_stream;
    }
    if (!metadata && !bytes) {
        if (f->mode == 1) n = (size_t)INT32_MAX + 1;
        if (f->mode == 2) n = 0;
    }
    *size = n;
    if (!bytes) return n ? MDKR_MOD_SOURCE_BUFFER_TOO_SMALL : MDKR_MOD_SOURCE_OK;
    if (!metadata) {
        ++f->data_reads;
        if (f->mode == 3) { *size = n + 1; return MDKR_MOD_SOURCE_BUFFER_TOO_SMALL; }
        if (f->mode == 4) --n;
    }
    if (cap < n) return MDKR_MOD_SOURCE_BUFFER_TOO_SMALL;
    memcpy(bytes, data, n);
    *size = n;
    return MDKR_MOD_SOURCE_OK;
}
static int write_file(const char *root, const char *name, const char *bytes) {
    char path[2048];
    if (snprintf(path, sizeof path, "%s/%s", root, name) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int okay = fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes);
    return fclose(f) == 0 && okay;
}
/* A scratch directory of this run's own beside CTest's base path. A fixed
 * name that one failed or killed run left behind would fail every later run
 * at its mkdir, and two runs sharing a build tree would race on it. */
static int make_unique_dir(const char *base, char *out, size_t size) {
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        if (snprintf(out, size, "%s.%ld.%u", base, process_id(), attempt) >= (int)size) return 0;
        if (make_dir(out) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}
static const char *const scratch_files[] = {"manifest.json", "header.bin", "texture.bin", "package.zip"};
/* Removes what the source cases may have written, then the directory itself;
 * runs whether they passed or not. */
static int remove_scratch(const char *dir) {
    for (unsigned i = 0; i < sizeof scratch_files / sizeof scratch_files[0]; ++i) {
        char path[2048];
        if (snprintf(path, sizeof path, "%s/%s", dir, scratch_files[i]) < (int)sizeof path)
            (void)remove(path);
    }
    return remove_dir(dir) == 0;
}
/* Real ZIP and directory sources, all inside `dir`. The archive is a file
 * there rather than tmpfile(), which on Windows targets the drive root and
 * fails outright for an ordinary non-admin account. */
static int source_cases(const char *dir, MdkrTrackPackageLimits limits,
                        const unsigned char digest[32]) {
    MdkrTrackPackageError error;
    MdkrCustomTrackPayload payload = {0};
    unsigned char compared[32];
    MdkrTrackPackage *p;

    /* Real ZIP source: CRC-checked payload reads, then source closure. */
    mz_zip_archive zip = {0};
    CHECK(mz_zip_writer_init_heap(&zip, 0, 0));
    CHECK(mz_zip_writer_add_mem(&zip, "manifest.json", manifest, strlen(manifest), MZ_BEST_COMPRESSION));
    CHECK(mz_zip_writer_add_mem(&zip, "header.bin", "HEAD", 4, MZ_BEST_COMPRESSION));
    CHECK(mz_zip_writer_add_mem(&zip, "texture.bin", "TEX", 3, MZ_NO_COMPRESSION));
    void *archive = NULL;
    size_t archive_size = 0;
    CHECK(mz_zip_writer_finalize_heap_archive(&zip, &archive, &archive_size));
    CHECK(mz_zip_writer_end(&zip));
    char archive_path[2048];
    CHECK(snprintf(archive_path, sizeof archive_path, "%s/package.zip", dir) < (int)sizeof archive_path);
    FILE *stream = mdkr_fopen_utf8(archive_path, "w+b");
    int written = stream && fwrite(archive, 1, archive_size, stream) == archive_size;
    mz_free(archive);
    if (!written && stream) fclose(stream);
    CHECK(written);
    rewind(stream);
    MdkrModSource *source = mdkr_mod_source_from_zip_stream(stream);
    CHECK(source);
    p = mdkr_track_package_from_source(source, limits, &error);
    mdkr_mod_source_close(source);
    CHECK(p && mdkr_track_package_payload(p, 1, &payload) && payload.size == 3 && !memcmp(payload.bytes, "TEX", 3));
    CHECK(mdkr_track_package_digest(p, compared) && !memcmp(digest, compared, 32));
    mdkr_track_package_destroy(p);

    /* Real directory source. */
    CHECK(write_file(dir, "manifest.json", manifest));
    CHECK(write_file(dir, "header.bin", "HEAD"));
    CHECK(write_file(dir, "texture.bin", "TEX"));
    source = mdkr_mod_source_open(dir, 0);
    CHECK(source);
    p = mdkr_track_package_from_source(source, limits, &error);
    mdkr_mod_source_close(source);
    CHECK(p);
    CHECK(write_file(dir, "header.bin", "CHANGED"));
    CHECK(mdkr_track_package_payload(p, 0, &payload) && payload.size == 4 && !memcmp(payload.bytes, "HEAD", 4));
    CHECK(mdkr_track_package_digest(p, compared) && !memcmp(digest, compared, 32));
    mdkr_track_package_destroy(p);
    CHECK(write_file(dir, "header.bin", "HEAd"));
    source = mdkr_mod_source_open(dir, 0);
    CHECK(source);
    p = mdkr_track_package_from_source(source, limits, &error);
    mdkr_mod_source_close(source);
    CHECK(p && mdkr_track_package_digest(p, compared) && memcmp(digest, compared, 32));
    mdkr_track_package_destroy(p);
    return 0;
}
typedef struct MapCheck { unsigned calls, fail_call; } MapCheck;
typedef struct CourseMapRead { unsigned calls, fail_call; uint32_t ids[2]; } CourseMapRead;
static int course_map_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    static const unsigned char map[34] = {
        [0]=24, [4]=9, [5]=1, [6]=24, [8]=231, [9]=255,
        [13]=8, [27]=8};
    CourseMapRead *state = context;
    if (state->calls >= 2) return 0;
    state->ids[state->calls++] = id;
    if (state->calls == state->fail_call) return 0;
    *out = (MdkrCustomTrackPayload){map, sizeof map};
    return 1;
}
static int map_check(void *context, const void *entries, size_t size) {
    MapCheck *state = context;
    const unsigned char *bytes = entries;
    ++state->calls;
    return state->calls != state->fail_call && size == 8 && bytes[0] == 0 && bytes[1] == 8;
}
typedef struct PreparedReaders { MiscFixture misc; CourseMapRead maps; } PreparedReaders;
static int prepared_misc_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    return misc_read(&((PreparedReaders *)context)->misc, id, out);
}
static int prepared_map_read(void *context, uint32_t id, MdkrCustomTrackPayload *out) {
    return course_map_read(&((PreparedReaders *)context)->maps, id, out);
}

int main(int argc, char **argv) {
    unsigned char course_header[MDKR_TRACK_HEADER_BYTES] = {[0xBB]=1}, map_scratch[24];
    for (unsigned variant = 0; variant < 6; ++variant) {
        CourseMapRead reader = {0};
        MapCheck behavior = {0};
        MdkrTrackObjectMapBudget budget = {99,99,99};
        if (variant == 1) reader.fail_call = 2;
        if (variant == 2) behavior.fail_call = 2;
        course_header[0x37] = variant == 4 ? 2 : variant == 5 ? 1 : 0;
        int accepted = mdkr_track_course_object_maps_admit(course_header, sizeof course_header,
            2, course_map_read, &reader, map_scratch, sizeof map_scratch, 1,
            variant == 3 ? 47 : 48, map_check, &behavior, &budget);
        CHECK(accepted == (variant == 0 || variant == 5));
        if (accepted) {
            CHECK(budget.entries == 2 && budget.records == 2 && budget.decoded_bytes == 48);
            CHECK(reader.calls == 2 && behavior.calls == 2);
            CHECK(reader.ids[0] == 1 && reader.ids[1] == (variant == 5 ? 1 : 0));
        } else CHECK(budget.entries == 99 && budget.records == 99 && budget.decoded_bytes == 99);
        if (variant == 4) CHECK(reader.calls == 0 && behavior.calls == 0);
        if (variant == 1 || variant == 3) CHECK(behavior.calls == 1);
    }
    MdkrTrackPackageLimits limits = {4, 7};
    MdkrTrackPackageError error;
    Fixture f = {0};
    MdkrTrackPackage *p = mdkr_track_package_read(fixture_read, &f, limits, &error);
    CHECK(p && error == MDKR_TRACK_PACKAGE_OK && f.calls == 6 && f.data_reads == 2);
    MdkrCustomTrackPayload payload = {0};
    CHECK(mdkr_track_package_payload(p, 0, &payload) && payload.size == 4 && !memcmp(payload.bytes, "HEAD", 4));
    MdkrCustomTrackPayload saved = payload;
    CHECK(!mdkr_track_package_payload(p, 2, &payload) && payload.bytes == saved.bytes && payload.size == saved.size);
    CHECK(!strcmp(mdkr_track_package_manifest(p)->id, "lake"));
    unsigned char digest[32], compared[32];
    static const unsigned char expected_digest[32] = {
        0xda,0x7e,0xd1,0x18,0xac,0xee,0x64,0x77,0x1a,0x88,0xc1,0x9a,0xa8,0x1a,0x95,0x11,
        0xe6,0x9b,0xb9,0x52,0x7c,0x6d,0xd8,0x20,0x80,0xa7,0x3f,0xa8,0xca,0xee,0x59,0xb9};
    /* Independently calculated with Python hashlib and BE length framing. */
    CHECK(mdkr_track_package_digest(p, digest) && !memcmp(digest, expected_digest, 32));
    memset(compared, 0xA5, sizeof compared);
    CHECK(!mdkr_track_package_digest(NULL, compared) && compared[0] == 0xA5 && compared[31] == 0xA5);
    CHECK(!mdkr_track_package_digest(p, NULL));
    const unsigned successful_allocations = allocations;
    const unsigned retained_allocations = live_allocations;
    CHECK(successful_allocations == 4 && retained_allocations == 3);
    for (unsigned allocation = 1; allocation <= successful_allocations; ++allocation) {
        Fixture bad = {0};
        allocations = 0;
        fail_allocation = allocation;
        CHECK(!mdkr_track_package_read(fixture_read, &bad, limits, &error));
        CHECK(error != MDKR_TRACK_PACKAGE_OK && live_allocations == retained_allocations);
        CHECK(!memcmp(saved.bytes, "HEAD", 4));
    }
    fail_allocation = 0;
    /* Failure of a separate candidate must not disturb existing owned data. */
    for (unsigned call = 1; call <= 6; ++call) {
        Fixture bad = {.fail_call = call};
        CHECK(mdkr_track_package_read(fixture_read, &bad, limits, &error) == NULL);
        CHECK(error != MDKR_TRACK_PACKAGE_OK && !memcmp(saved.bytes, "HEAD", 4));
    }
    const char *identity_manifests[] = {
        "{ \"adds\":[{\"file\":\"header.bin\",\"section\":\"LEVEL_HEADERS\"},{\"file\":\"texture.bin\",\"section\":\"TEXTURES_3D\"}],\"id\":\"lake\",\"schemaVersion\":1}",
        "{\"schemaVersion\":1,\"id\":\"lake\",\"name\":\"Changed\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"},{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"}]}",
        "{\"schemaVersion\":1,\"id\":\"lake\",\"adds\":[{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"},{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"}]}"};
    for (unsigned i = 0; i < 3; ++i) {
        Fixture identity = {.manifest_override = identity_manifests[i]};
        MdkrTrackPackage *candidate = mdkr_track_package_read(fixture_read, &identity, limits, &error);
        CHECK(candidate && mdkr_track_package_digest(candidate, compared));
        CHECK((memcmp(digest, compared, 32) == 0) == (i == 0));
        mdkr_track_package_destroy(candidate);
    }
    mdkr_track_package_destroy(p);
    for (unsigned mode = 1; mode <= 4; ++mode) {
        Fixture bad = {.mode = mode};
        CHECK(mdkr_track_package_read(fixture_read, &bad, limits, &error) == NULL);
    }
    f = (Fixture){0};
    CHECK(!mdkr_track_package_read(fixture_read, &f, (MdkrTrackPackageLimits){4, 6}, &error));
    CHECK(error == MDKR_TRACK_PACKAGE_BUDGET && f.data_reads == 0);
    f = (Fixture){0};
    CHECK(!mdkr_track_package_read(fixture_read, &f, (MdkrTrackPackageLimits){3, 7}, &error));
    CHECK(error == MDKR_TRACK_PACKAGE_BUDGET && f.data_reads == 0);
    CHECK(!mdkr_track_package_from_source(NULL, limits, &error) && error == MDKR_TRACK_PACKAGE_ARGUMENT);
    CHECK(mdkr_track_package_manifest(NULL) == NULL);
    mdkr_track_package_destroy(NULL);

    /* CTest names a base path; the run works in its own directory beside it. */
    char scratch[2048];
    CHECK(argc == 2 && make_unique_dir(argv[1], scratch, sizeof scratch));
    const int sources_failed = source_cases(scratch, limits, digest);
    const int scratch_removed = remove_scratch(scratch);
    CHECK(!sources_failed);
    CHECK(scratch_removed);
    CHECK(live_allocations == 0);
    for (unsigned mode = 5; mode <= 6; ++mode) {
        Fixture artwork = {.mode = mode};
        p = mdkr_track_package_read(fixture_read, &artwork,
            (MdkrTrackPackageLimits){64, 132}, &error);
        CHECK(p);
        MdkrTrackTextureBudget budget = {99, 99};
        CHECK(!mdkr_track_package_texture_budget(p, NULL, 0, 349, &budget));
        CHECK(budget.entries == 99 && budget.runtime_bytes == 99);
        if (mode == 5) {
            CHECK(mdkr_track_package_texture_budget(p, NULL, 0, 350, &budget));
            CHECK(budget.entries == 2 && budget.runtime_bytes == 350);
        } else {
            CHECK(!mdkr_track_package_texture_budget(p, NULL, 0, SIZE_MAX, &budget));
            CHECK(budget.entries == 99 && budget.runtime_bytes == 99);
        }
        CHECK(!mdkr_track_package_texture_budget(NULL, NULL, 0, 350, &budget));
        CHECK(!mdkr_track_package_texture_budget(p, NULL, 0, 350, NULL));
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    Fixture wrong_material = {.mode = 5, .invalid_texture_profile = 1};
    p = mdkr_track_package_read(fixture_read, &wrong_material,
        (MdkrTrackPackageLimits){256, 512}, &error);
    CHECK(p);
    MdkrTrackTextureBudget refused_budget = {99, 99};
    CHECK(!mdkr_track_package_texture_budget(p, NULL, 0, SIZE_MAX, &refused_budget));
    CHECK(refused_budget.entries == 99 && refused_budget.runtime_bytes == 99);
    mdkr_track_package_destroy(p);
    CHECK(live_allocations == 0);
    for (unsigned mode = 7; mode <= 8; ++mode) {
        Fixture localized = {.mode = mode};
        p = mdkr_track_package_read(fixture_read, &localized,
            (MdkrTrackPackageLimits){64, 64}, &error);
        CHECK(p);
        CHECK(mdkr_track_package_names_valid(p) == (mode == 7));
        unsigned char name[128], before[128];
        memset(name, 0xD5, sizeof name); memcpy(before, name, sizeof name);
        size_t name_size = 99;
        CHECK(mdkr_track_package_prepare_name(p, name, sizeof name, &name_size) ==
            (mode == 7 ? MDKR_TRACK_NAME_PAYLOAD : MDKR_TRACK_NAME_REFUSED));
        if (mode == 7) {
            static const char expected[] = "Lake\0Lac\0See\0Lago\0";
            CHECK(name_size == sizeof expected - 1 && !memcmp(name, expected, name_size));
            CHECK(!memcmp(name + name_size, before + name_size, sizeof name - name_size));
            memcpy(name, before, sizeof name);
            CHECK(mdkr_track_package_prepare_name(p, name, name_size - 1, &name_size) == MDKR_TRACK_NAME_REFUSED);
        }
        CHECK(name_size == 0 && !memcmp(name, before, sizeof name));
        const uint32_t retail_counts[] = {65, 136, 65, 55, 1416};
        const uint32_t section_limits[] = {128, 32768, 128, 32768, 32768};
        MdkrTrackPackageLayout names_layout;
        CHECK(mdkr_track_package_layout(p, retail_counts, retail_counts, section_limits, &names_layout));
        CHECK(names_layout.header.model == -1 && names_layout.header.structure == -1 &&
              names_layout.header.collectables == -1 && names_layout.texture_count == 0);
        CHECK(names_layout.counts[MDKR_CUSTOM_TRACK_NAMES] == 66 &&
              names_layout.counts[MDKR_CUSTOM_TRACK_HEADERS] == 65);
        MdkrTrackCourseLayout course, saved_course;
        memset(&course, 0xD5, sizeof course); memcpy(&saved_course, &course, sizeof course);
        CHECK(!mdkr_track_package_course_layout(p, retail_counts, retail_counts, section_limits, &course));
        CHECK(!memcmp(&course, &saved_course, sizeof course));
        mdkr_track_package_destroy(p);
    }
    CHECK(!mdkr_track_package_names_valid(NULL));
    Fixture named_course = {.mode = 7, .manifest_override =
        "{\"schemaVersion\":1,\"id\":\"named\",\"adds\":["
        "{\"section\":\"LEVEL_NAMES\",\"file\":\"names.bin\"},"
        "{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"}]}"};
    p = mdkr_track_package_read(fixture_read, &named_course,
        (MdkrTrackPackageLimits){64, 128}, &error);
    CHECK(p);
    {
        const uint32_t inventory[] = {65, 136, 65, 55, 1416};
        const uint32_t limits[] = {66, 136, 66, 55, 1416};
        MdkrTrackCourseLayout course;
        CHECK(mdkr_track_package_course_layout(p, inventory, inventory, limits, &course));
        CHECK(course.course_id == 65 && !course.synthesize_name &&
            course.assets.addition_ids[0] == 65 && course.assets.addition_ids[1] == 65 &&
            course.assets.counts[MDKR_CUSTOM_TRACK_NAMES] == 66 &&
            course.assets.counts[MDKR_CUSTOM_TRACK_HEADERS] == 66);
    }
    mdkr_track_package_destroy(p);
    const char *name_fields[] = {"", ",\"name\":\"Lake Run\"", ",\"name\":\"\"",
        ",\"name\":\"Lac\\u00e9\"", ",\"name\":\"Lake\\nRun\""};
    for (size_t i = 0; i < sizeof name_fields / sizeof name_fields[0]; ++i) {
        char metadata[512];
        snprintf(metadata, sizeof metadata,
            "{\"schemaVersion\":1,\"id\":\"lake\"%s,\"adds\":["
            "{\"section\":\"LEVEL_HEADERS\",\"file\":\"header.bin\"}]}", name_fields[i]);
        Fixture fallback = {.manifest_override = metadata};
        p = mdkr_track_package_read(fixture_read, &fallback,
            (MdkrTrackPackageLimits){64, 64}, &error);
        CHECK(p);
        unsigned char name[128], before[128];
        const char *expected = i == 1 ? "Lake Run" : "lake";
        const uint32_t inventory[] = {65, 136, 65, 55, 1416};
        uint32_t limits[] = {128, 32768, 128, 32768, 32768};
        MdkrTrackCourseLayout course, saved_course;
        CHECK(mdkr_track_package_course_layout(p, inventory, inventory, limits, &course));
        CHECK(course.course_id == 65 && course.synthesize_name &&
            course.assets.counts[MDKR_CUSTOM_TRACK_NAMES] == 66 &&
            course.assets.counts[MDKR_CUSTOM_TRACK_HEADERS] == 66);
        saved_course = course;
        CHECK(mdkr_track_package_course_layout(p, inventory, saved_course.assets.counts, limits, &course));
        CHECK(course.course_id == 66 && course.assets.counts[MDKR_CUSTOM_TRACK_NAMES] == 67);
        saved_course = course;
        limits[MDKR_CUSTOM_TRACK_NAMES] = 65;
        CHECK(!mdkr_track_package_course_layout(p, inventory, inventory, limits, &course));
        CHECK(!memcmp(&course, &saved_course, sizeof course));
        limits[MDKR_CUSTOM_TRACK_NAMES] = 128;
        uint32_t skewed[MDKR_CUSTOM_TRACK_SECTION_COUNT];
        memcpy(skewed, inventory, sizeof skewed); ++skewed[MDKR_CUSTOM_TRACK_NAMES];
        CHECK(!mdkr_track_package_course_layout(p, inventory, skewed, limits, &course));
        CHECK(!mdkr_track_package_course_layout(p, skewed, skewed, limits, &course));
        CHECK(!memcmp(&course, &saved_course, sizeof course));
        const size_t stride = strlen(expected) + 1, total = stride * 4;
        for (size_t cap = 0; cap <= total; ++cap) {
            memset(name, 0xD5, sizeof name); memcpy(before, name, sizeof name);
            size_t written = 99;
            MdkrTrackNameSource source = mdkr_track_package_prepare_name(p, name, cap, &written);
            if (cap < total) {
                CHECK(source == MDKR_TRACK_NAME_REFUSED && written == 0);
                CHECK(!memcmp(name, before, sizeof name));
            } else {
                CHECK(source == (i < 2 ? MDKR_TRACK_NAME_METADATA : MDKR_TRACK_NAME_IDENTIFIER));
                CHECK(written == total);
                for (unsigned language = 0; language < 4; ++language)
                    CHECK(!memcmp(name + language * stride, expected, stride));
                CHECK(!memcmp(name + total, before + total, sizeof name - total));
            }
        }
        size_t written = 99;
        CHECK(!mdkr_track_package_prepare_name(p, NULL, 128, &written) && written == 0);
        CHECK(!mdkr_track_package_prepare_name(p, name, sizeof name, NULL));
        written = 99;
        CHECK(!mdkr_track_package_prepare_name(NULL, name, sizeof name, &written) && written == 0);
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    Fixture layout_fixture = {.mode = 9};
    p = mdkr_track_package_read(fixture_read, &layout_fixture,
        (MdkrTrackPackageLimits){256, 512}, &error);
    CHECK(p);
    uint32_t retail[] = {65, 136, 65, 55, 1416};
    uint32_t limits_by_section[] = {128, 32768, 128, 32768, 32768};
    unsigned char staged_header[MDKR_TRACK_HEADER_BYTES + 4];
    MiscFixture misc = {0};
    size_t staged_size = 99;
    CHECK(mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
        2, 1, misc_read, &misc, staged_header, sizeof staged_header, &staged_size));
    CHECK(misc.calls == 8);
    CHECK(staged_size == sizeof staged_header && staged_header[0] == 1);
    CHECK(staged_header[0x34] == 0 && staged_header[0x35] == 55);
    CHECK(staged_header[0x36] == 0 && staged_header[0x37] == 136);
    CHECK(staged_header[0xBA] == 0 && staged_header[0xBB] == 137);
    CHECK(staged_header[MDKR_TRACK_HEADER_BYTES] == 0xA5);
    CHECK(mdkr_track_package_payload(p, 2, &payload));
    CHECK(((const unsigned char *)payload.bytes)[0] == 6 &&
          ((const unsigned char *)payload.bytes)[0x35] == 0);
    unsigned char saved_header[sizeof staged_header];
    memcpy(saved_header, staged_header, sizeof saved_header);
    misc = (MiscFixture){0};
    CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
        2, 0, misc_read, &misc, staged_header, sizeof staged_header, &staged_size));
    CHECK(misc.calls == 0 && staged_size == 0);
    CHECK(!memcmp(staged_header, saved_header, sizeof saved_header));
    for (unsigned call = 1; call <= 8; ++call) {
        for (unsigned truncate = 0; truncate < 2; ++truncate) {
            misc = (MiscFixture){.fail_call = truncate ? 0 : call,
                                 .short_call = truncate ? call : 0};
            CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
                2, 1, misc_read, &misc, staged_header, sizeof staged_header, &staged_size));
            CHECK(misc.calls == call && staged_size == 0);
            CHECK(!memcmp(staged_header, saved_header, sizeof saved_header));
        }
    }
    CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
        2, 1, NULL, NULL, staged_header, sizeof staged_header, &staged_size));
    CHECK(staged_size == 0 && !memcmp(staged_header, saved_header, sizeof saved_header));
    misc = (MiscFixture){0};
    CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
        2, 1, misc_read, &misc, staged_header, sizeof staged_header - 1, &staged_size));
    CHECK(staged_size == 0 && !memcmp(staged_header, saved_header, sizeof saved_header));
    CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
        0, 1, misc_read, &misc, staged_header, sizeof staged_header, &staged_size));
    CHECK(staged_size == 0 && !memcmp(staged_header, saved_header, sizeof saved_header));
    MdkrTrackPackageLayout layout;
    CHECK(mdkr_track_package_layout(p, retail, retail, limits_by_section, &layout));
    CHECK(layout.header.collectables == 136 && layout.header.structure == 137);
    CHECK(layout.header.model == 55 && layout.header.model_count == 56);
    CHECK(layout.header.object_map_count == 138 && layout.header.retail_object_map_count == 136);
    CHECK(layout.texture_count == 2 && layout.texture_ids[0] == 1416 && layout.texture_ids[1] == 1417);
    CHECK(layout.addition_ids[2] == 65 && layout.counts[MDKR_CUSTOM_TRACK_NAMES] == 65);
    /* Same package represents planning mechanics only: identity uniqueness is
     * a session-builder obligation. Counts may alias output for the next plan. */
    CHECK(mdkr_track_package_layout(p, retail, layout.counts, limits_by_section, &layout));
    CHECK(layout.header.model == 56 && layout.header.collectables == 138 && layout.header.structure == 139);
    CHECK(layout.texture_ids[0] == 1418 && layout.texture_ids[1] == 1419);
    CHECK(layout.header.retail_model_count == 55);
    MdkrTrackPackageLayout saved_layout = layout;
    limits_by_section[MDKR_CUSTOM_TRACK_TEXTURES] = 1417;
    CHECK(!mdkr_track_package_layout(p, retail, retail, limits_by_section, &layout));
    CHECK(!memcmp(&layout, &saved_layout, sizeof layout));
    limits_by_section[MDKR_CUSTOM_TRACK_TEXTURES] = UINT32_MAX;
    uint32_t edge[] = {65, 32766, 65, 32767, 32766};
    CHECK(mdkr_track_package_layout(p, retail, edge, limits_by_section, &layout));
    CHECK(layout.header.structure == 32767 && layout.texture_ids[1] == 32767);
    saved_layout = layout;
    edge[MDKR_CUSTOM_TRACK_TEXTURES]++;
    CHECK(!mdkr_track_package_layout(p, retail, edge, limits_by_section, &layout));
    CHECK(!memcmp(&layout, &saved_layout, sizeof layout));
    edge[MDKR_CUSTOM_TRACK_TEXTURES] = UINT32_MAX;
    CHECK(!mdkr_track_package_layout(p, retail, edge, limits_by_section, &layout));
    CHECK(!memcmp(&layout, &saved_layout, sizeof layout));
    CHECK(!mdkr_track_package_layout(NULL, retail, retail, limits_by_section, &layout));
    edge[MDKR_CUSTOM_TRACK_TEXTURES] = 1415;
    CHECK(!mdkr_track_package_layout(p, retail, edge, limits_by_section, &layout));
    CHECK(!memcmp(&layout, &saved_layout, sizeof layout));
    CHECK(!mdkr_track_package_layout(p, NULL, retail, limits_by_section, &layout));
    CHECK(!mdkr_track_package_layout(p, retail, NULL, limits_by_section, &layout));
    CHECK(!mdkr_track_package_layout(p, retail, retail, NULL, &layout));
    CHECK(!mdkr_track_package_layout(p, retail, retail, limits_by_section, NULL));
    mdkr_track_package_destroy(p);
    Fixture retained_fixture = {.mode = 10};
    p = mdkr_track_package_read(fixture_read, &retained_fixture,
        (MdkrTrackPackageLimits){256, 512}, &error);
    CHECK(p);
    /* Even if another package already appended model 55, an unrelocated
     * authored reference must not silently bind to that other course. */
    uint32_t prior_package_counts[] = {66, 138, 66, 56, 1418};
    CHECK(mdkr_track_package_layout(p, retail, prior_package_counts, limits_by_section, &layout));
    CHECK(!mdkr_track_package_prepare_header(p, retail, prior_package_counts,
        limits_by_section, 2, 1, misc_read, &misc, staged_header, sizeof staged_header, &staged_size));
    CHECK(staged_size == 0 && !memcmp(staged_header, saved_header, sizeof saved_header));
    CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
        2, 1, misc_read, &misc, staged_header, sizeof staged_header, NULL));
    mdkr_track_package_destroy(p);
    const unsigned refused_header_modes[] = {0, 7, 9}; /* short, absent, invalid vehicle */
    for (size_t i = 0; i < sizeof refused_header_modes / sizeof refused_header_modes[0]; ++i) {
        Fixture absent = {.mode = refused_header_modes[i], .invalid_vehicle = i == 2};
        p = mdkr_track_package_read(fixture_read, &absent,
            (MdkrTrackPackageLimits){256, 512}, &error);
        CHECK(p);
        CHECK(mdkr_track_package_layout(p, retail, retail, limits_by_section, &layout));
        staged_size = 99;
        CHECK(!mdkr_track_package_prepare_header(p, retail, retail, limits_by_section,
            2, 1, misc_read, &misc, staged_header, sizeof staged_header, &staged_size));
        CHECK(staged_size == 0 && !memcmp(staged_header, saved_header, sizeof saved_header));
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    for (unsigned local = 1; local <= 2; ++local) {
        Fixture source = {.mode = 14, .pulse_model = local};
        p = mdkr_track_package_read(fixture_read, &source,
            (MdkrTrackPackageLimits){256, 1024}, &error);
        CHECK(p);
        unsigned char model_scratch[512], output[512];
        PulseFixture texture = {.texture = {[0]=8, [1]=2, [2]=0x11, [0x12]=1, [0x17]=64}};
        MdkrTrackMaterialAdmission materials = {pulse_texture_read, &texture, NULL, 0};
        size_t written = 99;
        MdkrCustomTrackPayload pulse_model_payload;
        size_t pulse_decoded = 0, pulse_budget = 0;
        CHECK(mdkr_track_package_payload(p, 0, &pulse_model_payload));
        CHECK(mdkr_custom_track_decode(pulse_model_payload.bytes, pulse_model_payload.size,
            model_scratch, sizeof model_scratch, &pulse_decoded));
        CHECK(mdkr_track_model_validate_spans(model_scratch, pulse_decoded));
        CHECK(mdkr_track_model_validate_indices(model_scratch, pulse_decoded));
        CHECK(mdkr_track_model_validate_regions(model_scratch, pulse_decoded));
        CHECK(mdkr_track_model_collision_budget(model_scratch, pulse_decoded, &pulse_budget));
        CHECK(!mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, NULL, model_scratch, sizeof model_scratch,
            output, sizeof output, 4096, SIZE_MAX, &written));
        CHECK(written == 0 && texture.calls == 0);
        CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, &materials, model_scratch, sizeof model_scratch,
            output, sizeof output, 4096, SIZE_MAX, &written));
        CHECK(written > 0 && texture.calls == (local == 2));
        texture.refuse = 1;
        CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, &materials, model_scratch, sizeof model_scratch,
            output, sizeof output, 4096, SIZE_MAX, &written) == (local == 1));
        CHECK((written > 0) == (local == 1));
        if (local == 2) {
            Fixture header_source = {.mode = 9, .manifest_override =
                "{\"schemaVersion\":1,\"id\":\"pulse-retained\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"}]}"};
            MdkrTrackPackage *retained = mdkr_track_package_read(fixture_read, &header_source,
                (MdkrTrackPackageLimits){256, 512}, &error);
            CHECK(retained);
            texture.refuse = 0; texture.texture[2] = 0x11;
            CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                limits_by_section, 1, NULL, pulse_model_read, &pulse_model_payload,
                model_scratch, sizeof model_scratch, 4096, SIZE_MAX));
            CHECK(mdkr_track_package_retained_model_valid(retained, retail, retail,
                limits_by_section, 1, &materials, pulse_model_read, &pulse_model_payload,
                model_scratch, sizeof model_scratch, 4096, SIZE_MAX));
            texture.texture[2] = 0x14;
            CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                limits_by_section, 1, &materials, pulse_model_read, &pulse_model_payload,
                model_scratch, sizeof model_scratch, 4096, SIZE_MAX));
            mdkr_track_package_destroy(retained);
        }
        texture.refuse = 0; texture.texture[2] = 0x14; /* Valid bytes, wrong pulse format. */
        CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, &materials, model_scratch, sizeof model_scratch,
            output, sizeof output, 4096, SIZE_MAX, &written) == (local == 1));
        CHECK((written > 0) == (local == 1));
        mdkr_track_package_destroy(p);
        /* Independent stored DEFLATE block, including the outer texture
         * header: validate the selected payload through both resolution paths. */
        unsigned char compressed[106] = {0}, texture_scratch[66];
        texture.texture[2] = 0x11;
        memcpy(compressed, texture.texture, 32);
        compressed[0x1D] = 1;
        compressed[32] = 64; compressed[36] = 9;
        compressed[37] = 1; compressed[38] = 64;
        compressed[40] = 191; compressed[41] = 255;
        memcpy(compressed + 42, texture.texture, 64);
        source.pulse_texture = (MdkrCustomTrackPayload){compressed, sizeof compressed};
        texture.override = source.pulse_texture;
        p = mdkr_track_package_read(fixture_read, &source,
            (MdkrTrackPackageLimits){256, 1024}, &error);
        CHECK(p);
        memset(texture_scratch, 0xA5, sizeof texture_scratch);
        materials.scratch = texture_scratch + 1;
        for (size_t capacity = 0; capacity <= 64; ++capacity) {
            materials.scratch_capacity = capacity;
            written = 99;
            CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 1, &materials, model_scratch, sizeof model_scratch,
                output, sizeof output, 4096, SIZE_MAX, &written) == (capacity == 64));
            CHECK((written > 0) == (capacity == 64));
            CHECK(texture_scratch[0] == 0xA5 && texture_scratch[65] == 0xA5);
        }
        materials.scratch = NULL;
        CHECK(!mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, &materials, model_scratch, sizeof model_scratch,
            output, sizeof output, 4096, SIZE_MAX, &written));
        CHECK(written == 0);
        mdkr_track_package_destroy(p);
        compressed[42 + 2] = 0x14; /* Outer/decoded identity mismatch. */
        p = mdkr_track_package_read(fixture_read, &source,
            (MdkrTrackPackageLimits){256, 1024}, &error);
        CHECK(p);
        materials.scratch = texture_scratch + 1;
        CHECK(!mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, &materials, model_scratch, sizeof model_scratch,
            output, sizeof output, 4096, SIZE_MAX, &written));
        CHECK(written == 0 && texture_scratch[0] == 0xA5 && texture_scratch[65] == 0xA5);
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    for (unsigned mode = 11; mode <= 21; ++mode) {
        /* One segment, 2x2 tiles, four subdivisions, nine grid buckets.
         * Explicit allocation-by-allocation oracle, using target ABI sizes. */
        const size_t wave_peak = 28784 + 4 * 4 + sizeof(LevelModel_Alternate) +
            9 * 8 + 32 * sizeof(WaveGen) + 32 * sizeof(Object *) + 25 +
            4 + 4 * 8 + 5 * 4;
        Fixture model_fixture = {.mode = mode};
        p = mdkr_track_package_read(fixture_read, &model_fixture,
            (MdkrTrackPackageLimits){256, 512}, &error);
        CHECK(p);
        unsigned char scratch[256], encoded[512], decoded[256];
        size_t written = 99, decoded_size = 0;
        CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
            limits_by_section, 1, NULL, scratch, sizeof scratch, encoded, sizeof encoded,
            4096, wave_peak, &written) == (mode == 11 || mode == 14 || mode == 21));
        if (mode == 11 || mode == 14 || mode == 21) {
            CHECK(written > 5 && encoded[4] == 9);
            CHECK(mdkr_custom_track_decode(encoded, written, decoded, sizeof decoded, &decoded_size));
            CHECK(decoded_size == 0xC0 && decoded[0xBA] == 5 && decoded[0xBB] == 0x8A); /* 1418 */
            CHECK(mdkr_track_package_payload(p, 0, &payload));
            CHECK(((const unsigned char *)payload.bytes)[10 + 0xBA] == 0x70);
            CHECK(!mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 1, NULL, scratch, 0xBF, encoded, sizeof encoded, 4096, wave_peak, &written));
            CHECK(written == 0);
            CHECK(!mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 1, NULL, scratch, sizeof scratch, encoded, sizeof encoded, 128, wave_peak, &written));
            CHECK(written == 0);
            CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 1, NULL, scratch, sizeof scratch, encoded, sizeof encoded,
                4096, wave_peak - 1, &written) == (mode == 11));
            CHECK((written > 0) == (mode == 11));
            CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 0, NULL, scratch, sizeof scratch, encoded, sizeof encoded,
                4096, wave_peak, &written) == (mode == 11));
            CHECK((written > 0) == (mode == 11));
        } else CHECK(written == 0); /* Bad texture, header or water basis. */
        if (mode == 19) {
            /* Density doubles grid buckets and height-map resolution, not
             * base buffers: +64 bucket bytes, +56 heights, +16 scratch. */
            CHECK(mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 1, NULL, scratch, sizeof scratch, encoded, sizeof encoded,
                4096, wave_peak + 136, &written));
            CHECK(written > 0);
            CHECK(!mdkr_track_package_prepare_model(p, retail, prior_package_counts,
                limits_by_section, 1, NULL, scratch, sizeof scratch, encoded, sizeof encoded,
                4096, wave_peak + 135, &written));
            CHECK(written == 0);
        }
        if (mode == 14) {
            ModelFixture model = {0};
            CHECK(mdkr_track_package_payload(p, 0, &payload));
            CHECK(payload.size == sizeof model.bytes);
            memcpy(model.bytes, payload.bytes, payload.size);
            model.bytes[10 + 0xBA] = model.bytes[10 + 0xBB] = 0; /* Retail texture. */
            model.bytes[10 + 0xA4] |= 0x10; /* Requires header colour slot 1. */
            unsigned char original_model[sizeof model.bytes];
            memcpy(original_model, model.bytes, sizeof original_model);
            const char *header_only = "{\"schemaVersion\":1,\"id\":\"retained\",\"adds\":["
                "{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"}]}";
            const unsigned header_modes[] = {9, 10, 20};
            for (unsigned h = 0; h < 3; ++h) {
                Fixture header_fixture = {.mode = header_modes[h], .manifest_override = header_only};
                MdkrTrackPackage *retained = mdkr_track_package_read(fixture_read, &header_fixture,
                    (MdkrTrackPackageLimits){256, 512}, &error);
                CHECK(retained);
                model.calls = 0;
                CHECK(mdkr_track_package_retained_model_valid(retained, retail, retail,
                    limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                    4096, wave_peak) == (h == 0));
                CHECK(model.calls == (h == 1 ? 0 : 1));
                if (h == 0) {
                    const size_t exact_arena = sizeof model.bytes + 0xC0;
                    CHECK(mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                        exact_arena, wave_peak));
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                        exact_arena - 1, wave_peak));
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                        4096, wave_peak - 1));
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 0, NULL, model_read, &model, scratch, sizeof scratch,
                        4096, wave_peak));
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, 0xBF,
                        4096, wave_peak));
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                        128, wave_peak));
                    model.refuse = 1;
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                        4096, wave_peak));
                    model.refuse = 0;
                    model.bytes[10 + 0xBA] = 0x70; /* No local ordinals in retained models. */
                    CHECK(!mdkr_track_package_retained_model_valid(retained, retail, retail,
                        limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                        4096, wave_peak));
                    model.bytes[10 + 0xBA] = 0;
                }
                mdkr_track_package_destroy(retained);
            }
            model.calls = 0;
            CHECK(!mdkr_track_package_retained_model_valid(p, retail, retail,
                limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch, 4096, wave_peak));
            CHECK(model.calls == 0); /* A replacement model is not a retained-model request. */
            CHECK(!memcmp(model.bytes, original_model, sizeof original_model));
            const unsigned subdivision_cases[] = {8, 9, 127};
            for (unsigned s = 0; s < 3; ++s) {
                Gfx command;
                const unsigned triangles = 2 * subdivision_cases[s];
                gSPPolygon(&command, 0, triangles, TRIN_ENABLE_TEXTURE);
                const unsigned decoded_triangles = ((command.words.w0 >> 20) & 15) + 1;
                CHECK((decoded_triangles == triangles) == (s == 0));
            }
            for (unsigned s = 0; s < 3; ++s)
                for (unsigned retained_path = 0; retained_path < 2; ++retained_path) {
                    Fixture render_fixture = {.mode = 14, .wave_subdivisions = subdivision_cases[s],
                        .manifest_override = retained_path ? header_only : NULL};
                    MdkrTrackPackage *render_package = mdkr_track_package_read(fixture_read, &render_fixture,
                        (MdkrTrackPackageLimits){256, 512}, &error);
                    CHECK(render_package);
                    if (retained_path) {
                        CHECK(mdkr_track_package_retained_model_valid(render_package, retail, retail,
                            limits_by_section, 1, NULL, model_read, &model, scratch, sizeof scratch,
                            4096, SIZE_MAX) == (s == 0));
                    } else {
                        written = 99;
                        CHECK(mdkr_track_package_prepare_model(render_package, retail, retail,
                            limits_by_section, 1, NULL, scratch, sizeof scratch, encoded, sizeof encoded,
                            4096, SIZE_MAX, &written) == (s == 0));
                        CHECK((written > 0) == (s == 0));
                    }
                    mdkr_track_package_destroy(render_package);
                }
        }
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    for (unsigned mode = 22; mode <= 25; ++mode) {
        Fixture fixture = {.mode = mode};
        p = mdkr_track_package_read(fixture_read, &fixture,
            (MdkrTrackPackageLimits){256, 512}, &error);
        CHECK(p);
        unsigned char scratch[24];
        MdkrTrackObjectMapBudget result = {99, 99, 99};
        CHECK(mdkr_track_package_object_maps_valid(p, scratch, sizeof scratch, 1, 24,
            &result) == (mode == 22 || mode == 24));
        if (mode == 22 || mode == 24) {
            CHECK(result.entries == 1 && result.records == (mode == 24));
            CHECK(result.decoded_bytes == (mode == 24 ? 24 : 16));
            MdkrTrackObjectMapBudget saved = result;
            CHECK(!mdkr_track_package_object_maps_valid(p, scratch, sizeof scratch, 1,
                result.decoded_bytes - 1, &result));
            CHECK(!memcmp(&saved, &result, sizeof result));
            CHECK(!mdkr_track_package_object_maps_valid(p, scratch, saved.decoded_bytes - 1,
                1, 24, &result));
        } else CHECK(result.entries == 99 && result.records == 99 && result.decoded_bytes == 99);
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    for (unsigned mode = 26; mode <= 28; ++mode) {
        Fixture fixture = {.mode = mode};
        p = mdkr_track_package_read(fixture_read, &fixture,
            (MdkrTrackPackageLimits){256, 512}, &error);
        CHECK(p);
        unsigned char scratch[24];
        MdkrTrackObjectMapBudget result = {99, 99, 99};
        CHECK(mdkr_track_package_object_maps_valid(p, scratch, sizeof scratch, 1, 48,
            &result) == (mode == 26));
        if (mode == 26) {
            CHECK(result.entries == 2 && result.records == 2 && result.decoded_bytes == 48);
            MdkrTrackObjectMapBudget saved_result = result;
            MapCheck check = {0};
            CHECK(mdkr_track_package_object_maps_admit(p, scratch, sizeof scratch, 1,
                48, map_check, &check, &result));
            CHECK(check.calls == 2);
            for (unsigned failure = 1; failure <= 2; ++failure) {
                check = (MapCheck){0, failure};
                CHECK(!mdkr_track_package_object_maps_admit(p, scratch, sizeof scratch, 1,
                    48, map_check, &check, &result));
                CHECK(check.calls == failure && !memcmp(&result, &saved_result, sizeof result));
            }
            CHECK(!mdkr_track_package_object_maps_admit(p, scratch, sizeof scratch, 1,
                48, NULL, NULL, &result));
            CHECK(!memcmp(&result, &saved_result, sizeof result));
            for (size_t limit = 0; limit < 48; ++limit) {
                MdkrTrackObjectMapBudget saved = result;
                CHECK(!mdkr_track_package_object_maps_valid(p, scratch, sizeof scratch, 1,
                    limit, &result));
                CHECK(!memcmp(&saved, &result, sizeof result));
            }
        } else CHECK(result.entries == 99 && result.records == 99 && result.decoded_bytes == 99);
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    const char *course_manifests[] = {
        "{\"schemaVersion\":1,\"id\":\"course\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"},{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"map.bin\",\"slot\":\"collectables\"},{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"second.bin\",\"slot\":\"structure\"}]}",
        "{\"schemaVersion\":1,\"id\":\"course\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"},{\"section\":\"LEVEL_OBJECT_MAPS\",\"file\":\"map.bin\",\"slot\":\"collectables\"}]}",
        "{\"schemaVersion\":1,\"id\":\"course\",\"adds\":[{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"}]}"};
    for (unsigned variant = 0; variant < 4; ++variant) {
        Fixture fixture = {.mode=26, .course_maps=variant+1,
            .manifest_override=course_manifests[variant == 3 ? 1 : variant]};
        p = mdkr_track_package_read(fixture_read, &fixture,
            (MdkrTrackPackageLimits){256, 1024}, &error);
        CHECK(p);
        unsigned char scratch[24];
        CourseMapRead reader = {0};
        MapCheck check = {0};
        MdkrTrackObjectMapBudget result = {99,99,99};
        CHECK(mdkr_track_package_course_maps_admit(p, retail, prior_package_counts,
            limits_by_section, course_map_read, &reader, scratch, sizeof scratch, 1, 48,
            map_check, &check, &result) == (variant != 3));
        if (variant != 3) {
            CHECK(reader.calls == variant && check.calls == 2);
            CHECK(result.entries == 2 && result.records == 2 && result.decoded_bytes == 48);
            check = (MapCheck){0};
            CHECK(mdkr_track_package_course_maps_admit(p, retail, prior_package_counts,
                limits_by_section, NULL, NULL, scratch, sizeof scratch, 1, 48,
                map_check, &check, &result) == (variant == 0));
        } else {
            CHECK(reader.calls == 0 && check.calls == 0);
            CHECK(result.entries == 99 && result.records == 99 && result.decoded_bytes == 99);
        }
        CHECK(mdkr_track_package_payload(p, 0, &payload));
        CHECK(((const unsigned char *)payload.bytes)[0xBB] == (variant == 3 ? 137 : 0));
        mdkr_track_package_destroy(p);
    }
    CHECK(live_allocations == 0);
    Fixture prepared_source = {.mode=14, .pulse_model=1};
    p = mdkr_track_package_read(fixture_read, &prepared_source,
        (MdkrTrackPackageLimits){256, 1024}, &error);
    CHECK(p);
    unsigned char preparation_scratch[512];
    PreparedReaders readers = {0};
    MapCheck map_behavior = {0};
    MdkrTrackPreparation preparation = {0};
    memcpy(preparation.retail, retail, sizeof retail);
    memcpy(preparation.current, prior_package_counts, sizeof prior_package_counts);
    memcpy(preparation.limits, limits_by_section, sizeof limits_by_section);
    preparation.retail_misc_count = 2; preparation.retail_2d_count = 1;
    preparation.object_id_count = 1; preparation.engine_arena_bytes = 4096;
    preparation.wave_storage_limit = SIZE_MAX; preparation.texture_runtime_limit = 1024;
    preparation.decoded_map_limit = 48; preparation.owned_payload_limit = 8192;
    preparation.scratch = preparation_scratch; preparation.scratch_capacity = sizeof preparation_scratch;
    preparation.read_misc = prepared_misc_read; preparation.read_map = prepared_map_read;
    preparation.read_context = &readers; preparation.check_map = map_check;
    preparation.check_context = &map_behavior;
    unsigned before_prepared_allocations = allocations;
    MdkrPreparedCourse *prepared = mdkr_track_prepare_course(p, &preparation);
    CHECK(prepared && map_behavior.calls == 2 && readers.maps.calls == 2);
    unsigned preparation_allocations = allocations - before_prepared_allocations;
    unsigned owned_allocations = live_allocations;
    CHECK(mdkr_track_prepared_entry_count(prepared) == 4);
    CHECK(mdkr_track_prepared_layout(prepared)->course_id == prior_package_counts[MDKR_CUSTOM_TRACK_HEADERS]);
    CHECK(mdkr_track_package_digest(p, digest) && mdkr_track_prepared_digest(prepared, compared));
    CHECK(!memcmp(digest, compared, 32));
    for (unsigned failure = 1; failure <= preparation_allocations; ++failure) {
        allocations = 0; fail_allocation = failure;
        readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
        CHECK(!mdkr_track_prepare_course(p, &preparation));
        CHECK(live_allocations == owned_allocations);
    }
    fail_allocation = 0;
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
    const size_t prepared_capacity = 4096 + 200 + 64 + 4 * 256;
    preparation.owned_payload_limit = prepared_capacity;
    MdkrPreparedCourse *exact_prepared = mdkr_track_prepare_course(p, &preparation);
    CHECK(exact_prepared);
    mdkr_track_prepared_destroy(exact_prepared);
    CHECK(live_allocations == owned_allocations);
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
    preparation.owned_payload_limit = prepared_capacity - 1;
    CHECK(!mdkr_track_prepare_course(p, &preparation) && live_allocations == owned_allocations);
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0, 2};
    preparation.owned_payload_limit = prepared_capacity;
    CHECK(!mdkr_track_prepare_course(p, &preparation) && live_allocations == owned_allocations);
    CHECK(map_behavior.calls == 2);
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
    preparation.owned_payload_limit = 1;
    CHECK(!mdkr_track_prepare_course(p, &preparation) && live_allocations == owned_allocations);
    uint32_t retail_storage[MDKR_CUSTOM_TRACK_SECTION_COUNT][1420];
    MdkrTrackRetailTable retail_tables[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    size_t session_words = 0;
    for (unsigned kind = 0; kind < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++kind) {
        for (uint32_t i = 0; i <= retail[kind]; ++i) retail_storage[kind][i] = 4 * i;
        retail_storage[kind][retail[kind] + 1] = UINT32_MAX;
        retail_tables[kind] = (MdkrTrackRetailTable){retail_storage[kind], retail[kind] + 2};
        session_words += retail[kind] + 2;
    }
    const MdkrPreparedCourse *stale_list[] = {prepared};
    CHECK(!mdkr_track_prepared_session_create(stale_list, 1, retail_tables, 8192, session_words + 4));
    preparation.owned_payload_limit = 8192;
    memcpy(preparation.current, retail, sizeof retail);
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
    MdkrPreparedCourse *first_course = mdkr_track_prepare_course(p, &preparation);
    CHECK(first_course);
    memcpy(preparation.current, mdkr_track_prepared_layout(first_course)->assets.counts, sizeof retail);
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
    MdkrPreparedCourse *duplicate_course = mdkr_track_prepare_course(p, &preparation);
    CHECK(duplicate_course);
    const MdkrPreparedCourse *course_list[] = {first_course, duplicate_course};
    CHECK(!mdkr_track_prepared_session_create(course_list, 2, retail_tables, 8192, session_words + 8));
    mdkr_track_prepared_destroy(duplicate_course);
    prepared_source.manifest_override =
        "{\"schemaVersion\":1,\"id\":\"waves-next\",\"adds\":[{\"section\":\"LEVEL_MODELS\",\"file\":\"model.bin\"},{\"section\":\"LEVEL_HEADERS\",\"file\":\"c\"},{\"section\":\"TEXTURES_3D\",\"file\":\"texture.bin\"}]}";
    MdkrTrackPackage *second_package = mdkr_track_package_read(fixture_read, &prepared_source,
        (MdkrTrackPackageLimits){256, 1024}, &error);
    CHECK(second_package);
    readers = (PreparedReaders){0}; map_behavior = (MapCheck){0};
    MdkrPreparedCourse *second_course = mdkr_track_prepare_course(second_package, &preparation);
    CHECK(second_course);
    course_list[1] = second_course;
    size_t session_bytes = 0;
    for (unsigned i = 0; i < 2; ++i)
        for (size_t j = 0; j < mdkr_track_prepared_entry_count(course_list[i]); ++j) {
            MdkrPreparedCourseEntry entry;
            CHECK(mdkr_track_prepared_entry(course_list[i], j, &entry));
            session_bytes += entry.payload.size;
        }
    CHECK(!mdkr_track_prepared_session_create(course_list, 2, retail_tables, session_bytes - 1, session_words + 8));
    CHECK(!mdkr_track_prepared_session_create(course_list, 2, retail_tables, session_bytes, session_words + 7));
    const MdkrPreparedCourse *reversed[] = {second_course, first_course};
    CHECK(!mdkr_track_prepared_session_create(reversed, 2, retail_tables, session_bytes, session_words + 8));
    const unsigned before_session_allocations = allocations;
    MdkrCustomTrackSession *prepared_session = mdkr_track_prepared_session_create(
        course_list, 2, retail_tables, session_bytes, session_words + 8);
    CHECK(prepared_session);
    const unsigned session_allocations = allocations - before_session_allocations;
    const unsigned session_live_allocations = live_allocations;
    for (unsigned failure = 1; failure <= session_allocations; ++failure) {
        allocations = 0; fail_allocation = failure;
        CHECK(!mdkr_track_prepared_session_create(course_list, 2, retail_tables,
            session_bytes, session_words + 8));
        CHECK(live_allocations == session_live_allocations);
    }
    fail_allocation = 0;
    mdkr_track_prepared_destroy(first_course);
    mdkr_track_prepared_destroy(second_course);
    mdkr_track_package_destroy(second_package);
    const MdkrCustomTrackSection *name_section = mdkr_custom_track_session_section(prepared_session, MDKR_CUSTOM_TRACK_NAMES);
    size_t name_words = 0;
    const uint32_t *name_table = mdkr_custom_track_section_table(name_section, &name_words);
    CHECK(name_words == retail[MDKR_CUSTOM_TRACK_NAMES] + 4);
    char copied_name[44];
    CHECK(mdkr_custom_track_section_read(name_section, name_table[retail[MDKR_CUSTOM_TRACK_NAMES] + 1],
        sizeof copied_name, copied_name, sizeof copied_name) == MDKR_CUSTOM_TRACK_SPAN_CUSTOM);
    CHECK(!memcmp(copied_name, "waves-next\0waves-next\0waves-next\0waves-next\0", sizeof copied_name));
    mdkr_custom_track_session_destroy(prepared_session);
    mdkr_track_package_destroy(p);
    /* Prepared model, header, texture and synthesized name survive source destruction. */
    MdkrPreparedCourseEntry prepared_entry;
    CHECK(mdkr_track_prepared_entry(prepared, 1, &prepared_entry));
    CHECK(prepared_entry.section == MDKR_CUSTOM_TRACK_HEADERS);
    CHECK(((const unsigned char *)prepared_entry.payload.bytes)[0x35] == prior_package_counts[MDKR_CUSTOM_TRACK_MODELS]);
    CHECK(mdkr_track_prepared_entry(prepared, 3, &prepared_entry));
    CHECK(prepared_entry.section == MDKR_CUSTOM_TRACK_NAMES && prepared_entry.payload.size == 24);
    CHECK(!memcmp(prepared_entry.payload.bytes, "waves\0waves\0waves\0waves\0", 24));
    MdkrPreparedCourseEntry saved_prepared_entry = prepared_entry;
    CHECK(!mdkr_track_prepared_entry(prepared, 4, &prepared_entry));
    CHECK(prepared_entry.payload.bytes == saved_prepared_entry.payload.bytes);
    mdkr_track_prepared_destroy(prepared);
    CHECK(live_allocations == 0);
    puts("custom track package: PASS");
    return 0;
}
