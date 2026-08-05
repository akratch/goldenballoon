#ifndef _GZIP_H_
#define _GZIP_H_

#include "types.h"

typedef struct huft {
  u8 e;                /* number of extra bits or operation */
  u8 b;                /* number of bits in this code or subcode */
  union {
    u16 n;              /* literal, length base, or distance base */
    struct huft *t;     /* pointer to next level of table */
  } v;
} huft;

/* If BMAX needs to be larger than 16, then h and x[] should be ulg. */
#define BMAX 16         /* maximum bit length of any code (16 for explode) */
#define N_MAX 288       /* maximum number of codes in any set */

void gzip_init(void);
u32 byteswap32(u8 *arg0);
s32 gzip_size_uncompressed(s32 assetIndex, s32 assetOffset);
u8 *gzip_inflate(u8 *compressedInput, u8 *decompressedOutput);
#ifdef NATIVE_PORT
/* Same decode, with the caller's own extent for the compressed buffer.
 *
 * A bare `u8 *` carries no length, so the hardened decoder had to recover one
 * from the pool slot that owns the address (mempool_block_end) and fell back to
 * the output bound alone whenever the input was not pool-resident. Every caller
 * in the tree already knows the exact compressed span it just DMA'd, so it
 * passes it here instead of leaving the input bound to be inferred.
 *
 * `compressedSize` is the byte length of the buffer starting at
 * `compressedInput`, including its 5-byte rzip header. <= 0 means "unknown",
 * which keeps the mempool_block_end fallback. */
u8 *gzip_inflate_sized(u8 *compressedInput, u8 *decompressedOutput,
                       s32 compressedSize);
/* Points just past the last decompressed byte after gzip_inflate() returns; the
 * NATIVE_PORT asset-swap hooks use it to size the just-inflated buffer. */
extern u8 *gzip_inflate_output;
#else
void gzip_huft_build(u32 *b, u32 n, u32 s, u16 *d, u16 *e, huft **t, s32 *m);
#endif
/* Decodes one DEFLATE block: > 0 while more blocks follow, 0 after the final
 * block, < 0 when the stream is rejected (NATIVE_PORT decoder only). */
s32 gzip_inflate_block(void);

#endif
