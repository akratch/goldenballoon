#ifndef MDKR_SPRITE_LAYOUT_H
#define MDKR_SPRITE_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

#include "structs.h"

/*
 * Host-side layout of a loaded Sprite allocation.
 *
 * Sprite assets contain a variable-length cumulative frame-to-texture table.
 * The generated display-list command count is likewise variable: every tile
 * emits a texture command and polygon command, every group of at most five
 * tiles emits a vertex command, and every frame emits a sync/end pair.
 *
 * Keeping the arithmetic in a pure helper makes the allocation contract
 * independently testable and prevents the builder from silently walking into
 * the following region.
 */
typedef struct SpriteBuildLayout {
    size_t triangle_offset;
    size_t display_list_offset;
    size_t vertex_offset;
    size_t texture_offset;
    size_t total_size;
    size_t display_list_count;
    size_t triangle_count;
    size_t vertex_count;
    size_t texture_count;
    size_t frame_count;
} SpriteBuildLayout;

/*
 * Derive the complete in-memory Sprite layout.
 *
 * asset_size is the exact serialized asset extent. It is used to prove that
 * frameTexOffsets[frame_count] exists before reading the flexible tail.
 */
bool sprite_build_layout(const SpriteAsset *asset, size_t asset_size,
                         SpriteBuildLayout *layout);

/* Exact number of Gfx commands emitted for one frame with tile_count tiles. */
bool sprite_frame_command_count(size_t tile_count, size_t *command_count);

#endif
