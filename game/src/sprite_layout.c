#include "sprite_layout.h"

#include <stdint.h>
#include <string.h>

static bool checked_add(size_t left, size_t right, size_t *result) {
    if (result == NULL || left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool checked_multiply(size_t left, size_t right, size_t *result) {
    if (result == NULL || (left != 0 && right > SIZE_MAX / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool checked_align(size_t value, size_t alignment, size_t *result) {
    size_t mask;

    if (result == NULL || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        return false;
    }
    mask = alignment - 1;
    if (value > SIZE_MAX - mask) {
        return false;
    }
    *result = (value + mask) & ~mask;
    return true;
}

static bool append_region(size_t *cursor, size_t count, size_t element_size,
                          size_t following_alignment) {
    size_t bytes;
    size_t end;

    return cursor != NULL &&
           checked_multiply(count, element_size, &bytes) &&
           checked_add(*cursor, bytes, &end) &&
           checked_align(end, following_alignment, cursor);
}

bool sprite_frame_command_count(size_t tile_count, size_t *command_count) {
    size_t tile_commands;
    size_t vertex_commands;
    size_t total;

    if (command_count == NULL ||
        !checked_multiply(tile_count, 2, &tile_commands) ||
        !checked_add(tile_count, 4, &vertex_commands)) {
        return false;
    }
    vertex_commands /= 5;
    if (!checked_add(2, tile_commands, &total) ||
        !checked_add(total, vertex_commands, &total)) {
        return false;
    }
    *command_count = total;
    return true;
}

bool sprite_build_layout(const SpriteAsset *asset, size_t asset_size,
                         SpriteBuildLayout *layout) {
    const size_t offsets_start = offsetof(SpriteAsset, frameTexOffsets);
    size_t offsets_size;
    size_t offsets_end;
    size_t frame_count;
    size_t texture_count;
    size_t display_list_count = 0;
    size_t cursor;

    if (asset == NULL || layout == NULL || asset->numberOfFrames <= 0) {
        return false;
    }

    frame_count = (size_t)asset->numberOfFrames;
    if (!checked_add(frame_count, 1, &offsets_size) ||
        !checked_add(offsets_start, offsets_size, &offsets_end) ||
        offsets_end > asset_size ||
        asset->frameTexOffsets[0] != 0) {
        return false;
    }

    for (size_t frame = 0; frame < frame_count; frame++) {
        size_t first = asset->frameTexOffsets[frame];
        size_t next = asset->frameTexOffsets[frame + 1];
        size_t frame_commands;

        if (next < first ||
            !sprite_frame_command_count(next - first, &frame_commands) ||
            !checked_add(display_list_count, frame_commands,
                         &display_list_count)) {
            return false;
        }
    }
    texture_count = asset->frameTexOffsets[frame_count];

    memset(layout, 0, sizeof(*layout));
    layout->frame_count = frame_count;
    layout->texture_count = texture_count;
    layout->display_list_count = display_list_count;
    if (!checked_multiply(texture_count, 2, &layout->triangle_count) ||
        !checked_multiply(texture_count, 4, &layout->vertex_count)) {
        return false;
    }

    cursor = offsetof(Sprite, frames);
    if (!append_region(&cursor, frame_count, sizeof(Gfx *), 16)) {
        return false;
    }
    layout->triangle_offset = cursor;

    if (!append_region(&cursor, layout->triangle_count, sizeof(Triangle), 16)) {
        return false;
    }
    layout->display_list_offset = cursor;

    if (!append_region(&cursor, display_list_count, sizeof(Gfx), 16)) {
        return false;
    }
    layout->vertex_offset = cursor;

    if (!append_region(&cursor, layout->vertex_count, sizeof(Vertex),
                       _Alignof(TextureHeader *))) {
        return false;
    }
    layout->texture_offset = cursor;

    if (!append_region(&cursor, texture_count, sizeof(TextureHeader *), 16)) {
        return false;
    }
    layout->total_size = cursor;
    return true;
}
