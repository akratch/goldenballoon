#ifndef MDKR_CUSTOM_TRACK_DECODE_H
#define MDKR_CUSTOM_TRACK_DECODE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Decode the game's five-byte rzip wrapper (LE length, opaque byte, raw
 * deflate) into caller-owned staging memory. Capacity is an independent
 * admission budget, not a size trusted from the stream. Requires exact output
 * length; permits trailing compressed-section alignment padding as retail does.
 * Returns 1 on success, 0 on malformed input/insufficient capacity. On failure
 * written is zero but staging bytes may be modified; never publish that buffer.
 * Input/output must not overlap. This validates compression, not asset records. */
int mdkr_custom_track_decode(const void *input, size_t input_size,
                             void *output, size_t capacity, size_t *written);
#ifdef __cplusplus
}
#endif
#endif
