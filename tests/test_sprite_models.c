#include "sprite_models.h"
#include "mod_texture_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* The content-pack master switch is independent of atlas ownership. */
static bool packs_enabled = true;
bool mdkr_mod_texture_enabled(void) { return packs_enabled; }
/* The renderer's capability, as gfx_pc_dkr.c reports it: no game renderer in
 * the launcher, no model path on OpenGL. */
static int backend_supported = 1;
int gfx_sprite_models_supported(void) { return backend_supported; }
/* The renderer's retirement hooks: record order, and whether the model
 * storage they retire was still alive when they ran. */
static int retire_calls[2], retire_storage_alive[2], retire_order_ok = 1;
void gfx_dkr_replay_invalidate(void) {
    ++retire_calls[0];
    retire_storage_alive[0] += mdkr_sprite_models_mesh(115, 0) != NULL;
}
void gfx_clear_modern_mesh_cache(void) {
    ++retire_calls[1];
    retire_storage_alive[1] += mdkr_sprite_models_mesh(115, 0) != NULL;
    if (retire_calls[1] > retire_calls[0]) retire_order_ok = 0;
}
static int failures;
static void check(int yes, const char *why) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}

static long long peak_rss_bytes(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
#ifdef __APPLE__
    return (long long)usage.ru_maxrss;
#else
    return (long long)usage.ru_maxrss * 1024LL;
#endif
}

/* A decompression bomb: a 64x64 atlas whose IDAT inflates to hundreds of
 * megabytes. The header admits it, so only a bound on the decoder's own
 * allocations keeps the reject from costing that memory first. */
static int run_bomb(MdkrModRegistry *registry, const char *root) {
    const long long before = peak_rss_bytes();
    check(mdkr_mod_registry_init(registry, root) == 0, "scan bomb pack");
    mdkr_sprite_models_init(registry);
    const long long grown = peak_rss_bytes() - before;
    check(mdkr_sprite_models_count() == 0 && !mdkr_sprite_models_ready(115),
          "an atlas that inflates past its header is refused");
    fprintf(stderr, "bomb: peak resident growth %lld KiB\n", grown / 1024);
    check(before > 0 && grown < 48LL * 1024 * 1024,
          "refusing the bomb allocated no more than the header allows");
    mdkr_sprite_models_shutdown();
    mdkr_mod_registry_shutdown(registry);
    return failures != 0;
}

/* Every family present: the five balloon colours and both collectibles share
 * one mesh per file, both canopy sizes share theirs, and each shared file is
 * parsed once. */
static void check_families(MdkrModRegistry *registry, const char *root) {
    static const unsigned balloons[] = {147, 148, 149, 150, 151};
    check(mdkr_mod_registry_init(registry, root) == 0, "scan family pack");
    mdkr_sprite_models_init(registry);
    check(mdkr_sprite_models_count() == 14, "all fourteen sprite replacements load");
    /* palm near/far + canopy near/far, balloons near/far, collectible
     * near/far, banana near/far, beach-tree, blueberry and rubber-tree
     * near/far: twelve-plus-six distinct files. */
    check(mdkr_sprite_models_geometry_parses() == 16, "each distinct mesh file is parsed once");
    for (unsigned lod = 0; lod < 2; ++lod) {
        struct GfxModernMesh *first = mdkr_sprite_models_mesh(balloons[0], (int)lod);
        check(first != NULL && first->geometry_id != 0, "balloon mesh carries a geometry identity");
        if (!first) continue;
        for (unsigned i = 1; i < 5; ++i) {
            struct GfxModernMesh *other = mdkr_sprite_models_mesh(balloons[i], (int)lod);
            check(other && other->geometry_id == first->geometry_id && other->vtx == first->vtx &&
                  other->idx == first->idx, "balloon colours share one parsed mesh");
            check(other && other->mesh_id != first->mesh_id && other->texture_id != first->texture_id,
                  "each balloon colour keeps its own atlas and draw identity");
        }
        struct GfxModernMesh *canopy = mdkr_sprite_models_mesh(114, (int)lod);
        struct GfxModernMesh *small = mdkr_sprite_models_mesh(116, (int)lod);
        check(canopy && small && canopy->geometry_id == small->geometry_id && canopy->vtx == small->vtx,
              "both canopy sizes share one parsed mesh");
        struct GfxModernMesh *palm = mdkr_sprite_models_mesh(115, (int)lod);
        check(palm && canopy && palm->geometry_id != canopy->geometry_id, "distinct files stay distinct");
        struct GfxModernMesh *c154 = mdkr_sprite_models_mesh(154, (int)lod);
        struct GfxModernMesh *c155 = mdkr_sprite_models_mesh(155, (int)lod);
        check(c154 && c155 && c154->geometry_id == c155->geometry_id &&
              c154->geometry_id != first->geometry_id, "collectibles share their own mesh");
    }
    struct GfxModernMesh *near = mdkr_sprite_models_mesh(147, 0), *far = mdkr_sprite_models_mesh(147, 1);
    check(near && far && near->geometry_id != far->geometry_id, "near and far are separate files");
    mdkr_sprite_models_shutdown();
    check(mdkr_sprite_models_geometry_parses() == 0 && !mdkr_sprite_models_mesh(147, 0),
          "shutdown releases shared geometry");
    /* A backend that cannot draw models decodes nothing, though it can still
     * say which families the packs supply. */
    backend_supported = 0;
    mdkr_sprite_models_init(registry);
    check(mdkr_sprite_models_count() == 0 && mdkr_sprite_models_geometry_parses() == 0 &&
          !mdkr_sprite_models_ready(147), "no decode where models cannot draw");
    check(mdkr_sprite_models_present(registry) == 14, "supplied models are counted without decoding");
    backend_supported = 1;
    mdkr_sprite_models_shutdown();
    mdkr_mod_registry_shutdown(registry);
}

int main(int argc, char **argv) {
    MdkrModRegistry *registry;
    uint32_t previous_texture = 0;
    int cycle, split, partial, rejected;
    if (argc != 3) return 2;
    split = !strcmp(argv[2], "split");
    partial = !strcmp(argv[2], "partial");
    rejected = !strcmp(argv[2], "bad-atlas") || !strcmp(argv[2], "bad-mesh");
    registry = calloc(1, sizeof(*registry));
    if (!registry) return 2;
    if (!strcmp(argv[2], "bomb")) {
        const int result = run_bomb(registry, argv[1]);
        free(registry);
        if (!result) puts("sprite_models: decompression bomb refused within its header's bound");
        return result;
    }
    if (!strcmp(argv[2], "families")) {
        check_families(registry, argv[1]);
        free(registry);
        if (failures) return 1;
        puts("sprite_models: shared meshes parse once; nothing decodes without a model path");
        return 0;
    }
    for (cycle = 0; cycle < 3; ++cycle) {
        struct GfxModernMesh *palm, *canopy, *small;
        check(mdkr_mod_registry_init(registry, argv[1]) == 0, "scan fixture packs");
        mdkr_sprite_models_init(registry);
        if (rejected) {
            check(mdkr_sprite_models_count() == 0, "malformed families load nothing");
            check(!mdkr_sprite_models_ready(115) && !mdkr_sprite_models_mesh(115, 0),
                  "malformed families preserve billboard fallback");
            mdkr_sprite_models_shutdown();
            mdkr_sprite_models_shutdown();
            mdkr_mod_registry_shutdown(registry);
            continue;
        }
        check(mdkr_sprite_models_count() == (partial ? 1 : 3), "load only complete families");
        palm = mdkr_sprite_models_mesh(115, 0);
        check(palm && palm->texture_id > previous_texture, "reload never reuses a retired atlas identity");
        if (!palm) return 1;
        previous_texture = palm->texture_id;
        check(palm->tex_rgba[0] == 255 && palm->tex_rgba[2] == 0, "full palm uses its selected pack");
        check(palm->texture_id == mdkr_sprite_models_mesh(115, 1)->texture_id &&
              palm->tex_rgba == mdkr_sprite_models_mesh(115, 1)->tex_rgba, "LODs share decoded atlas and GPU identity");
        canopy = mdkr_sprite_models_mesh(114, 0);
        small = mdkr_sprite_models_mesh(116, 0);
        if (partial) {
            check(!canopy && !small, "incomplete priority pack falls back to sprites, not mixed assets");
        } else {
            check(canopy && small, "both canopy sizes load");
            if (!canopy || !small) return 1;
            check(canopy->texture_id == small->texture_id && canopy->tex_rgba == small->tex_rgba,
                  "canopy sizes share one atlas");
            check((canopy->texture_id == palm->texture_id) == !split &&
                  (canopy->tex_rgba == palm->tex_rgba) == !split,
                  "sharing requires both path and originating pack to match");
            check(canopy->tex_rgba[2] == (split ? 255 : 0), "higher priority canopy keeps its own artwork");
        }
        mdkr_sprite_models_toggle();
        check(!mdkr_sprite_models_ready(115), "off restores original sprite admission");
        check(mdkr_sprite_models_mesh(115, 0) == palm, "retained commands keep the authored mesh after toggle");
        mdkr_sprite_models_toggle();
        check(mdkr_sprite_models_ready(115), "on restores model admission");
        packs_enabled = false;
        check(!mdkr_sprite_models_enabled() && !mdkr_sprite_models_ready(115),
              "master pack switch gates new replacement commands");
        check(mdkr_sprite_models_mesh(115, 0) == palm,
              "master pack switch preserves already-authored mesh ownership");
        mdkr_sprite_models_toggle();
        mdkr_sprite_models_toggle();
        check(!mdkr_sprite_models_ready(115),
              "model toggle cannot override disabled master packs");
        packs_enabled = true;
        check(mdkr_sprite_models_ready(115), "master pack restore needs no asset reload");
        memset(retire_calls, 0, sizeof(retire_calls));
        memset(retire_storage_alive, 0, sizeof(retire_storage_alive));
        mdkr_sprite_models_shutdown();
        check(!mdkr_sprite_models_mesh(115, 0) && !mdkr_sprite_models_count(), "shutdown retires all slots");
        check(retire_calls[0] == 1 && retire_calls[1] == 1 && retire_order_ok,
              "shutdown retires retained lists, then the GPU cache, once each");
        check(retire_storage_alive[0] == 1 && retire_storage_alive[1] == 1,
              "the renderer is retired while the model storage it names is still alive");
        mdkr_sprite_models_shutdown();
        check(retire_calls[0] == 1 && retire_calls[1] == 1,
              "shutting down nothing retires nothing");
        mdkr_mod_registry_shutdown(registry);
    }
    free(registry);
    if (failures) return 1;
    puts("sprite_models: atlas sharing, pack isolation and reload checks passed");
    return 0;
}
