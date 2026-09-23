#include "sprite_models.h"
#include "mod_texture_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The content-pack master switch is independent of atlas ownership. */
static bool packs_enabled = true;
bool mdkr_mod_texture_enabled(void) { return packs_enabled; }
static int failures;
static void check(int yes, const char *why) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", why); ++failures; }
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
        mdkr_sprite_models_shutdown();
        check(!mdkr_sprite_models_mesh(115, 0) && !mdkr_sprite_models_count(), "shutdown retires all slots");
        mdkr_mod_registry_shutdown(registry);
    }
    free(registry);
    if (failures) return 1;
    puts("sprite_models: atlas sharing, pack isolation and reload checks passed");
    return 0;
}
