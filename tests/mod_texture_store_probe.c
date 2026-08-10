/* mod_texture_store_probe.c — see mod_texture_store_probe.h.
 *
 * This is the ONE translation unit that instantiates stb_image and
 * stb_image_write for tests/test_mod_texture_store.c, standing in for
 * lib/stb/stb_image_impl.c. The configuration below must stay identical to
 * that file's (STBI_ONLY_PNG / STBI_NO_STDIO / STBI_WRITE_NO_STDIO), because
 * platform/mod_texture_store.c repeats the same configuration when it takes
 * the declarations and the three have to agree about which entry points exist.
 * The only differences are the two seams this file adds:
 *
 *   STBI_MALLOC/REALLOC/FREE route the decoder's allocations through a counter
 *   (and, optionally, a refusal) so a test can read the largest allocation the
 *   decoder ever asked for.
 *
 *   stbi_load_from_memory is renamed while the implementation is compiled, and
 *   a wrapper of the original name is defined below it. Every caller -- the
 *   store included -- links to the wrapper, which counts and then delegates.
 *   Nothing about the decode itself changes; the real stb code runs.
 *
 * Third-party code is compiled here, so this file is built with warnings off,
 * exactly as lib/stb/stb_image_impl.c and lib/miniz/miniz.c are.
 */
#include "mod_texture_store_probe.h"

#include <stdlib.h>

static size_t s_largest_request;
static size_t s_refuse_above;
static int    s_decode_calls;

static void *probe_malloc(size_t size) {
    if (size > s_largest_request) s_largest_request = size;
    if (s_refuse_above != 0 && size > s_refuse_above) return NULL;
    return malloc(size);
}

static void *probe_realloc(void *pointer, size_t size) {
    if (size > s_largest_request) s_largest_request = size;
    if (s_refuse_above != 0 && size > s_refuse_above) return NULL;
    return realloc(pointer, size);
}

#define STBI_MALLOC(sz)        probe_malloc((size_t)(sz))
#define STBI_REALLOC(p, newsz) probe_realloc((p), (size_t)(newsz))
#define STBI_FREE(p)           free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define stbi_load_from_memory mdkr_texture_probe_real_decode
#include "stb_image.h"
#undef stbi_load_from_memory

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* The name every caller links to. Declared here because stb_image.h declared
 * the renamed symbol instead of this one. */
unsigned char *stbi_load_from_memory(unsigned char const *buffer, int len,
                                     int *x, int *y, int *comp, int req_comp);

unsigned char *stbi_load_from_memory(unsigned char const *buffer, int len,
                                     int *x, int *y, int *comp, int req_comp) {
    s_decode_calls++;
    return mdkr_texture_probe_real_decode(buffer, len, x, y, comp, req_comp);
}

void mdkr_texture_probe_reset(size_t refuse_above) {
    s_largest_request = 0;
    s_refuse_above = refuse_above;
    s_decode_calls = 0;
}

size_t mdkr_texture_probe_largest_request(void) {
    return s_largest_request;
}

int mdkr_texture_probe_decode_calls(void) {
    return s_decode_calls;
}
