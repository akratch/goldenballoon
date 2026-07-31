#include "sprite_layout.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

static void expect_true(const char *name, bool condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_size(const char *name, size_t actual, size_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: got %zu, expected %zu\n",
                name, actual, expected);
        s_failures++;
    }
}

static SpriteAsset *make_asset(const unsigned char *offsets, size_t count,
                               size_t *asset_size) {
    size_t bytes = offsetof(SpriteAsset, frameTexOffsets) + count;
    SpriteAsset *asset = calloc(1, bytes);

    if (asset == NULL) {
        fprintf(stderr, "fatal: allocation failed\n");
        exit(2);
    }
    asset->numberOfFrames = (s16)(count - 1);
    memcpy(asset->frameTexOffsets, offsets, count);
    *asset_size = bytes;
    return asset;
}

static void test_command_counts(void) {
    size_t count = 0;

    expect_true("empty frame command count",
                sprite_frame_command_count(0, &count));
    expect_size("empty frame emits sync and end", count, 2);

    expect_true("single tile command count",
                sprite_frame_command_count(1, &count));
    expect_size("single tile emits five commands", count, 5);

    expect_true("six tile command count",
                sprite_frame_command_count(6, &count));
    expect_size("six tiles need two vertex loads", count, 16);

    expect_true("command output required",
                !sprite_frame_command_count(1, NULL));
    expect_true("command overflow rejected",
                !sprite_frame_command_count(SIZE_MAX, &count));
}

static void test_exact_layout(void) {
    const unsigned char offsets[] = { 0, 2, 8 };
    SpriteBuildLayout layout;
    size_t asset_size;
    SpriteAsset *asset = make_asset(offsets, sizeof(offsets), &asset_size);

    expect_true("valid two-frame layout",
                sprite_build_layout(asset, asset_size, &layout));
    expect_size("frame count", layout.frame_count, 2);
    expect_size("texture count", layout.texture_count, 8);
    expect_size("triangle count", layout.triangle_count, 16);
    expect_size("vertex count", layout.vertex_count, 32);
    /* frame 0: 2 + 4 + 1; frame 1: 2 + 12 + 2 */
    expect_size("display-list count", layout.display_list_count, 23);
    expect_true("triangles aligned", layout.triangle_offset % 16 == 0);
    expect_true("display lists aligned", layout.display_list_offset % 16 == 0);
    expect_true("vertices aligned", layout.vertex_offset % 16 == 0);
    expect_true("texture pointers aligned",
                layout.texture_offset % _Alignof(TextureHeader *) == 0);
    expect_true("regions strictly ordered",
                layout.triangle_offset < layout.display_list_offset &&
                layout.display_list_offset < layout.vertex_offset &&
                layout.vertex_offset < layout.texture_offset &&
                layout.texture_offset < layout.total_size);
    free(asset);
}

static void test_known_old_underrun_shape(void) {
    /*
     * Seven frames share five tiles, including two deliberately empty frames.
     * The old allocation reserved 4*T + F = 27 commands. The real writer emits
     * 2*F + 2*T + sum(ceil(frameTiles/5)) = 29.
     */
    const unsigned char offsets[] = { 0, 0, 1, 2, 3, 4, 5, 5 };
    SpriteBuildLayout layout;
    size_t asset_size;
    SpriteAsset *asset = make_asset(offsets, sizeof(offsets), &asset_size);
    size_t old_count = 4 * 5 + 7;

    expect_true("old-underrun shape accepted",
                sprite_build_layout(asset, asset_size, &layout));
    expect_size("old reservation", old_count, 27);
    expect_size("exact reservation", layout.display_list_count, 29);
    expect_true("positive control proves underrun",
                layout.display_list_count > old_count);
    free(asset);
}

static void test_malformed_assets(void) {
    const unsigned char valid_offsets[] = { 0, 1, 2 };
    const unsigned char descending_offsets[] = { 0, 3, 2 };
    const unsigned char nonzero_start[] = { 1, 2 };
    SpriteBuildLayout layout;
    size_t asset_size;
    SpriteAsset *asset =
        make_asset(valid_offsets, sizeof(valid_offsets), &asset_size);

    expect_true("truncated flexible tail rejected",
                !sprite_build_layout(asset, asset_size - 1, &layout));
    asset->numberOfFrames = 0;
    expect_true("zero frames rejected",
                !sprite_build_layout(asset, asset_size, &layout));
    free(asset);

    asset = make_asset(descending_offsets, sizeof(descending_offsets),
                       &asset_size);
    expect_true("descending offsets rejected",
                !sprite_build_layout(asset, asset_size, &layout));
    free(asset);

    asset = make_asset(nonzero_start, sizeof(nonzero_start), &asset_size);
    expect_true("nonzero first offset rejected",
                !sprite_build_layout(asset, asset_size, &layout));
    free(asset);

    expect_true("null asset rejected",
                !sprite_build_layout(NULL, 0, &layout));
    asset = make_asset(valid_offsets, sizeof(valid_offsets), &asset_size);
    expect_true("null layout rejected",
                !sprite_build_layout(asset, asset_size, NULL));
    free(asset);
}

int main(void) {
    test_command_counts();
    test_exact_layout();
    test_known_old_underrun_shape();
    test_malformed_assets();

    if (s_failures != 0) {
        fprintf(stderr, "%d sprite layout test(s) failed\n", s_failures);
        return 1;
    }
    printf("sprite layout tests passed\n");
    return 0;
}
