/* mod_texture_store_probe.h — what the pack-texture store asked the PNG
 * decoder for, observed from underneath it.
 *
 * tests/test_mod_texture_store.c has to distinguish two claims that look the
 * same from the outside: "an oversized pack texture is rejected" (true before
 * the fix as well, after the whole picture had been allocated) and "an
 * oversized pack texture is rejected before it is allocated". Only the second
 * one is the defence against a malformed pack, and nothing the store returns
 * tells the two apart -- both come back as a failed lookup.
 *
 * So the test links this translation unit in place of lib/stb/stb_image_impl.c.
 * It is the same stb_image, configured identically, with two seams added:
 * every allocation the decoder makes passes through a counter, and the decode
 * entry point is wrapped in one. A store that consults the declared size first
 * enters neither for a picture it refuses.
 */
#ifndef MDKR64_TEST_MOD_TEXTURE_STORE_PROBE_H
#define MDKR64_TEST_MOD_TEXTURE_STORE_PROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zeroes both counters. `refuse_above`, when non-zero, makes the decoder's
 * allocator return NULL for any single request larger than it -- the request
 * is still recorded, so the test reads the size that WOULD have been taken
 * without the machine having to find it. That is what lets the deliberately
 * broken build be run safely: a gigabyte is measured, never allocated. */
void   mdkr_texture_probe_reset(size_t refuse_above);

/* The largest single allocation the decoder has asked for since the reset. */
size_t mdkr_texture_probe_largest_request(void);

/* How many times stbi_load_from_memory() has been entered since the reset. */
int    mdkr_texture_probe_decode_calls(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_TEST_MOD_TEXTURE_STORE_PROBE_H */
