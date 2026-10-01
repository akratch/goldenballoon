/* stb_image_impl.c — the one translation unit that instantiates stb_image and
 * stb_image_write.
 *
 * Both headers are header-only: exactly one TU in the program may define
 * STB_IMAGE_IMPLEMENTATION / STB_IMAGE_WRITE_IMPLEMENTATION, and this is it.
 * Keeping that TU separate from the code that uses the decoder/encoder is what
 * lets CMake compile third-party source with warnings off
 * (set_source_files_properties(... COMPILE_OPTIONS -w)) without weakening
 * -Wall -Wextra -Werror for any first-party file. Never fold this back into
 * platform/mod_texture_store.c.
 *
 * The configuration is deliberately narrow. Texture-pack, character-texture,
 * compiled-portrait and Portrait Studio callers repeat it before taking the
 * declarations; every caller must agree about which entry points exist:
 *
 *   STBI_ONLY_PNG        Content packs ship PNG. Every other decoder in the
 *                        file (JPEG, BMP, TGA, PSD, GIF, HDR, PIC, PNM) is
 *                        code that would be linked in, and attack surface
 *                        that would be reachable, for a format nothing here
 *                        reads.
 *   STBI_NO_STDIO        The store reads the file itself, through the port's
 *                        own UTF-8 filesystem boundary, and hands the decoder
 *                        bytes. Letting stb open paths would put a second,
 *                        narrow-CRT path API in the tree.
 *   STBI_WRITE_NO_STDIO  Same reasoning, the write direction: the author-dump
 *                        path (MDKR_MOD_TEXTURE_DUMP) and native capture writer
 *                        encode into memory via stbi_write_png_to_func() and write the result
 *                        through that same UTF-8-safe boundary, so the
 *                        header's own fopen()-based *_write_png() etc. must
 *                        not exist to be reached by accident.
 *
 * STBI_NO_FAILURE_STRINGS is deliberately NOT set: stbi_failure_reason() is
 * what puts the actual defect in a bad PNG into the log line the pack author
 * has to act on. stb_image_write has no equivalent switch.
 *
 * The first-party decoder allocator rejects empty requests and preserves
 * ownership on refused resize. Positive sizes are refused only above the
 * calling thread's decode bound (mdkr_stbi_set_allocation_limit), which
 * callers derive from the PNG header before decoding a file from a pack;
 * without one the caller's admission bounds remain responsible for image
 * budgets. This does not modify the pinned headers or the writer's
 * allocation policy.
 */
#include "stb_image_alloc.h"
MDKR_STBI_ALLOCATION_LIMIT_STORAGE
#define STBI_MALLOC(sz)        mdkr_stbi_malloc((size_t)(sz))
#define STBI_REALLOC(p, newsz) mdkr_stbi_realloc((p), (size_t)(newsz))
#define STBI_FREE(p)           free(p)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
