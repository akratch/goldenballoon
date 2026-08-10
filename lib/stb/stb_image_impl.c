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
 * The configuration is deliberately narrow, and platform/mod_texture_store.c
 * repeats it before taking the declarations — the two must agree about which
 * entry points exist:
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
 *                        path (MDKR_MOD_TEXTURE_DUMP) encodes into memory via
 *                        stbi_write_png_to_func() and writes the result
 *                        through that same UTF-8-safe boundary, so the
 *                        header's own fopen()-based *_write_png() etc. must
 *                        not exist to be reached by accident.
 *
 * STBI_NO_FAILURE_STRINGS is deliberately NOT set: stbi_failure_reason() is
 * what puts the actual defect in a bad PNG into the log line the pack author
 * has to act on. stb_image_write has no equivalent switch.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
