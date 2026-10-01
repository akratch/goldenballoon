/*
 * texture_pack_decode_bench.c -- what a whole Rice/GLideN64 texture pack
 * costs to resolve on one thread (issue #66 sizing).
 *
 * Not part of the game build. tools/measure_texture_pack_stalls.py
 * --inventory compiles it with the host compiler against the store's own
 * sources (the same registry, mod_source, miniz and stb_image the game links)
 * and runs it over an isolated mods directory holding one pack. It answers
 * the questions a prefetch or background-decode design has to be sized from:
 *
 *   open    what one pack open + entry probe costs on its own. The store opens
 *           the pack once per file it looks for (mdkr_mod_registry_open_file),
 *           absent probes included, so this is paid per texture regardless of
 *           how small the texture is.
 *   engine  every identity the pack indexes, resolved through the store's real
 *           lookup (mdkr_mod_texture_lookup_rice) with its resolve timing on:
 *           what "decode everything now" costs with today's resolver.
 *   stream  the same files read through ONE open source and decoded with the
 *           same stb_image call: the floor a resolver that keeps the archive
 *           open would pay, split into read+inflate and PNG decode.
 *
 * Output is one JSON object on stdout. Usage:
 *   texture_pack_decode_bench <mods-dir> [open-probe-repeats]
 */
#include "mod_registry.h"
#include "mod_source.h"
#include "mod_texture_store.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

typedef struct StreamTotals {
    uint64_t files;
    uint64_t failures;
    uint64_t file_bytes;
    uint64_t decoded_bytes;
    uint64_t read_ns;
    uint64_t decode_ns;
} StreamTotals;

static void stream_one(MdkrModSource *source, const char *relative,
                       StreamTotals *totals) {
    size_t size = 0;
    unsigned char *bytes;
    unsigned char *pixels;
    int width = 0, height = 0, channels = 0;
    uint64_t start = now_ns();
    uint64_t middle;
    int result = mdkr_mod_source_read(source, relative, NULL, 0, &size);

    if ((result != MDKR_MOD_SOURCE_OK &&
         result != MDKR_MOD_SOURCE_BUFFER_TOO_SMALL) || size == 0) {
        totals->failures++;
        return;
    }
    bytes = (unsigned char *)malloc(size);
    if (bytes == NULL ||
        mdkr_mod_source_read(source, relative, bytes, size, &size) !=
            MDKR_MOD_SOURCE_OK) {
        free(bytes);
        totals->failures++;
        return;
    }
    middle = now_ns();
    pixels = stbi_load_from_memory(bytes, (int)size, &width, &height,
                                   &channels, 4);
    totals->decode_ns += now_ns() - middle;
    totals->read_ns += middle - start;
    free(bytes);
    if (pixels == NULL) {
        totals->failures++;
        return;
    }
    totals->files++;
    totals->file_bytes += size;
    totals->decoded_bytes += (uint64_t)width * (uint64_t)height * 4u;
    stbi_image_free(pixels);
}

int main(int argc, char **argv) {
    MdkrModRegistry registry;
    MdkrModTextureResolveTiming engine;
    StreamTotals stream;
    MdkrModSource *source;
    const MdkrModEntry *pack;
    int repeats = argc > 2 ? atoi(argv[2]) : 200;
    int index;
    int resident = 0;
    uint64_t open_ns = 0;
    uint64_t engine_wall;
    uint64_t start;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <mods-dir> [open-probe-repeats]\n", argv[0]);
        return 2;
    }
    if (repeats < 1) repeats = 1;
    if (mdkr_mod_registry_init(&registry, argv[1]) != 0 ||
        registry.count < 1 || registry.rice_count < 1) {
        fprintf(stderr, "no Rice pack indexed under %s\n", argv[1]);
        return 1;
    }

    /* 1. One pack open + probe for a name no pack holds: the per-file
     *    overhead every resolution pays at least once (digest probe) and,
     *    for a Rice texture, a second time to read it. */
    for (index = 0; index < repeats; index++) {
        MdkrModFile file;
        start = now_ns();
        if (mdkr_mod_registry_open_file(
                &registry, "textures/00000000000000000000000000000000.png",
                &file)) {
            mdkr_mod_registry_close_file(&file);
        }
        open_ns += now_ns() - start;
    }

    /* 2. Every identity through the store's own resolver. */
    mdkr_mod_texture_store_init(&registry);
    mdkr_mod_texture_timing_set_clock(now_ns);
    start = now_ns();
    for (index = 0; index < registry.rice_count; index++) {
        const MdkrModRiceTexture *texture = &registry.rice[index];
        MdkrModTexture out;
        resident += mdkr_mod_texture_lookup_rice(texture->crc, texture->fmt,
                                                 texture->siz, &out);
    }
    engine_wall = now_ns() - start;
    mdkr_mod_texture_timing_totals(&engine);
    mdkr_mod_texture_timing_set_clock(NULL);
    mdkr_mod_texture_store_shutdown();

    /* 3. The same files through one open source. */
    memset(&stream, 0, sizeof stream);
    pack = mdkr_mod_registry_entry(&registry, registry.rice[0].pack);
    source = pack != NULL ? mdkr_mod_source_open(pack->root, pack->is_zip)
                          : NULL;
    if (source == NULL) {
        fprintf(stderr, "could not reopen the pack for the stream pass\n");
        mdkr_mod_registry_shutdown(&registry);
        return 1;
    }
    for (index = 0; index < registry.rice_count; index++) {
        const MdkrModRiceTexture *texture = &registry.rice[index];
        if (texture->pack != registry.rice[0].pack) continue;
        if (texture->all != NULL) {
            stream_one(source, texture->all, &stream);
        } else {
            if (texture->rgb != NULL) stream_one(source, texture->rgb, &stream);
            if (texture->alpha != NULL) {
                stream_one(source, texture->alpha, &stream);
            }
        }
    }
    mdkr_mod_source_close(source);

    printf("{\"identities\": %d, \"open_probe\": {\"repeats\": %d, "
           "\"mean_ms\": %.4f}, "
           "\"engine\": {\"wall_ms\": %.2f, \"resolves\": %llu, "
           "\"loaded\": %d, \"files\": %llu, \"file_bytes\": %llu, "
           "\"decoded_bytes\": %llu, \"open_ms\": %.2f, \"read_ms\": %.2f, "
           "\"decode_ms\": %.2f, \"resolve_ms\": %.2f}, "
           "\"stream\": {\"files\": %llu, \"failures\": %llu, "
           "\"file_bytes\": %llu, \"decoded_bytes\": %llu, "
           "\"read_ms\": %.2f, \"decode_ms\": %.2f}}\n",
           registry.rice_count, repeats, ms(open_ns) / repeats,
           ms(engine_wall), (unsigned long long)engine.resolves, resident,
           (unsigned long long)engine.files,
           (unsigned long long)engine.file_bytes,
           (unsigned long long)engine.decoded_bytes, ms(engine.open_ns),
           ms(engine.read_ns), ms(engine.decode_ns), ms(engine.resolve_ns),
           (unsigned long long)stream.files,
           (unsigned long long)stream.failures,
           (unsigned long long)stream.file_bytes,
           (unsigned long long)stream.decoded_bytes, ms(stream.read_ns),
           ms(stream.decode_ns));
    mdkr_mod_registry_shutdown(&registry);
    return 0;
}
