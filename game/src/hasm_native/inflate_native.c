/*
 * hasm_native/inflate_native.c
 *
 * Native C replacement for the handwritten-asm DEFLATE decoder in
 * game/src/hasm/gzip_asm.s.  The asm file implements:
 *     gzip_inflate_block  (+ internal gzip_inflate_stored / _fixed / _dynamic /
 *                          _codes, which build tables via gzip_huft_build)
 * driven by gzip_inflate() in game/src/gzip.c.
 *
 * The only symbol the rest of the game references is gzip_inflate_block(); the
 * stored/fixed/dynamic/codes helpers are internal.  We therefore reimplement
 * just gzip_inflate_block() with a compact, self-contained puff-style inflate
 * (public-domain puff.c logic, reimplemented; no external deps) and no longer
 * use the gHuftTable machinery in gzip.c.
 *
 * Stream format (see gzip.c / DKR "rzip"): gzip_inflate() strips a 5-byte
 * header, points gzip_inflate_input at a *raw DEFLATE* stream, points
 * gzip_inflate_output at a buffer pre-sized to the full uncompressed length,
 * and calls gzip_inflate_block() until it returns 0.  Bits are consumed
 * LSB-first through the shared 32-bit bit buffer.  Because the whole output is
 * one contiguous buffer, LZ77 back-references copy directly from earlier in the
 * output (there is no separate sliding window), matching the asm exactly.
 *
 * Contract preserved from the asm:
 *   - gzip_inflate_block() decodes exactly one DEFLATE block, advancing the
 *     shared input/output pointers and bit buffer, and returns (1 - BFINAL):
 *     nonzero while more blocks follow, 0 once the final block is decoded.
 *   - Stored blocks discard the partial bits to reach a byte boundary, read the
 *     16-bit LEN and 16-bit NLEN (NLEN ignored, as in the asm), then copy LEN
 *     raw bytes straight from the input pointer.
 *
 * Endianness: operates on the byte-defined compressed stream and produces the
 * byte-defined decompressed stream; endianness normalization of decompressed
 * *assets* happens later, in the asset-load swap layer (never here).
 */

#include "gzip.h"

/* Shared decode state, defined in game/src/gzip.c. */
extern u8 *gzip_inflate_input;
extern u8 *gzip_inflate_output;
extern u32 gzip_bit_buffer;
extern u32 gzip_num_bits;

#define MAXBITS 15   /* max bits in a Huffman code */
#define MAXLCODES 286 /* max number of literal/length codes */
#define MAXDCODES 30  /* max number of distance codes */
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288 /* number of fixed literal/length codes */

struct huffman {
    short *count;  /* number of symbols of each length */
    short *symbol; /* canonically ordered symbols */
};

/* Read a single bit, LSB-first, refilling the shared buffer a byte at a time. */
static int getbit(void) {
    int bit;
    if (gzip_num_bits == 0) {
        gzip_bit_buffer |= (u32) (*gzip_inflate_input++);
        gzip_num_bits = 8;
    }
    bit = (int) (gzip_bit_buffer & 1u);
    gzip_bit_buffer >>= 1;
    gzip_num_bits--;
    return bit;
}

/* Read `need` bits (0..16) as an unsigned value, LSB-first. */
static u32 getbits(int need) {
    u32 val;
    while (gzip_num_bits < (u32) need) {
        gzip_bit_buffer |= (u32) (*gzip_inflate_input++) << gzip_num_bits;
        gzip_num_bits += 8;
    }
    val = gzip_bit_buffer & ((1u << need) - 1u);
    gzip_bit_buffer >>= need;
    gzip_num_bits -= (u32) need;
    return val;
}

/* Decode one symbol using the canonical Huffman table `h`. */
static int decode(const struct huffman *h) {
    int len;
    int code = 0;
    int first = 0;
    int index = 0;

    for (len = 1; len <= MAXBITS; len++) {
        int count;
        code |= getbit();
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -9; /* ran out of codes */
}

/*
 * Build a canonical Huffman decode table from a list of code lengths.
 * Returns 0 for a complete code, >0 for an incomplete code (unused slots), or
 * <0 for an over-subscribed code.
 */
static int construct(struct huffman *h, const short *length, int n) {
    int symbol;
    int len;
    int left;
    short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (symbol = 0; symbol < n; symbol++) {
        h->count[length[symbol]]++;
    }
    if (h->count[0] == n) {
        return 0; /* no codes at all -> complete (empty) */
    }

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left; /* over-subscribed */
        }
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (symbol = 0; symbol < n; symbol++) {
        if (length[symbol] != 0) {
            h->symbol[offs[length[symbol]]++] = symbol;
        }
    }
    return left;
}

/* Length/distance base values and extra-bit counts (RFC 1951). */
static const short kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,   23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                     6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

/* Decode literal/length + distance codes straight into the output buffer. */
static int codes(const struct huffman *lencode, const struct huffman *distcode) {
    int symbol;

    do {
        symbol = decode(lencode);
        if (symbol < 0) {
            return symbol;
        }
        if (symbol < 256) {
            *gzip_inflate_output++ = (u8) symbol;
        } else if (symbol > 256) {
            int len;
            u32 dist;
            u8 *from;

            symbol -= 257;
            if (symbol >= 29) {
                return -10; /* invalid length code */
            }
            len = kLenBase[symbol] + (int) getbits(kLenExtra[symbol]);

            symbol = decode(distcode);
            if (symbol < 0) {
                return symbol;
            }
            dist = (u32) kDistBase[symbol] + getbits(kDistExtra[symbol]);

            /* Contiguous output buffer -> copy back-reference in place. */
            from = gzip_inflate_output - dist;
            while (len--) {
                *gzip_inflate_output++ = *from++;
            }
        }
    } while (symbol != 256); /* 256 == end of block */

    return 0;
}

/* BTYPE 0: stored (uncompressed) block. */
static void inflate_stored(void) {
    u32 len;

    /* Discard remaining bits in the current partial byte. */
    gzip_bit_buffer >>= (gzip_num_bits & 7u);
    gzip_num_bits -= (gzip_num_bits & 7u);

    len = getbits(16);  /* LEN  */
    (void) getbits(16); /* NLEN (one's complement of LEN, unchecked as in asm) */

    while (len--) {
        *gzip_inflate_output++ = *gzip_inflate_input++;
    }
}

/* BTYPE 1: fixed Huffman block. */
static int inflate_fixed(void) {
    static short lencnt[MAXBITS + 1];
    static short lensym[FIXLCODES];
    static short distcnt[MAXBITS + 1];
    static short distsym[MAXDCODES];
    static struct huffman lencode;
    static struct huffman distcode;
    static int built = 0;

    if (!built) {
        short lengths[FIXLCODES];
        int symbol;

        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        lencode.count = lencnt;
        lencode.symbol = lensym;
        construct(&lencode, lengths, FIXLCODES);

        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        distcode.count = distcnt;
        distcode.symbol = distsym;
        construct(&distcode, lengths, MAXDCODES);

        built = 1;
    }

    return codes(&lencode, &distcode);
}

/* Order in which the code-length code lengths are stored (RFC 1951). */
static const short kCodeLengthOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                           11, 4,  12, 3, 13, 2, 14, 1, 15};

/* BTYPE 2: dynamic Huffman block. */
static int inflate_dynamic(void) {
    int nlen;
    int ndist;
    int ncode;
    int index;
    int err;
    short lengths[MAXCODES];
    short lencnt[MAXBITS + 1];
    short lensym[MAXLCODES];
    short distcnt[MAXBITS + 1];
    short distsym[MAXDCODES];
    struct huffman lencode;
    struct huffman distcode;

    lencode.count = lencnt;
    lencode.symbol = lensym;
    distcode.count = distcnt;
    distcode.symbol = distsym;

    nlen = (int) getbits(5) + 257;
    ndist = (int) getbits(5) + 1;
    ncode = (int) getbits(4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) {
        return -3; /* bad counts */
    }

    /* Read the code-length code lengths and build their Huffman table. */
    for (index = 0; index < ncode; index++) {
        lengths[kCodeLengthOrder[index]] = (short) getbits(3);
    }
    for (; index < 19; index++) {
        lengths[kCodeLengthOrder[index]] = 0;
    }
    err = construct(&lencode, lengths, 19);
    if (err != 0) {
        return -4; /* require a complete code-length code */
    }

    /* Read the literal/length and distance code lengths. */
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(&lencode);
        if (symbol < 0) {
            return symbol;
        }
        if (symbol < 16) {
            lengths[index++] = (short) symbol;
        } else {
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) {
                    return -5; /* no previous length to repeat */
                }
                repeat = lengths[index - 1];
                symbol = 3 + (int) getbits(2);
            } else if (symbol == 17) {
                symbol = 3 + (int) getbits(3);
            } else { /* symbol == 18 */
                symbol = 11 + (int) getbits(7);
            }
            if (index + symbol > nlen + ndist) {
                return -6; /* too many lengths */
            }
            while (symbol--) {
                lengths[index++] = (short) repeat;
            }
        }
    }

    /* Build the literal/length and distance Huffman tables.  An incomplete code
     * is tolerated only in the single-code case (matches puff/gzip). */
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) {
        return -7;
    }
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) {
        return -8;
    }

    return codes(&lencode, &distcode);
}

/*
 * Decode one DEFLATE block.  Returns nonzero while more blocks follow, 0 after
 * decoding the final (BFINAL) block -- the exact loop contract gzip_inflate()
 * relies on (`while (gzip_inflate_block() != 0) {}`).
 */
s32 gzip_inflate_block(void) {
    int last = (int) getbits(1); /* BFINAL */
    int type = (int) getbits(2); /* BTYPE  */

    if (type == 0) {
        inflate_stored();
    } else if (type == 1) {
        (void) inflate_fixed();
    } else if (type == 2) {
        (void) inflate_dynamic();
    }
    /* type == 3 is reserved/invalid; the original would corrupt output, so we
     * simply produce nothing for it. */

    return last ? 0 : 1;
}
