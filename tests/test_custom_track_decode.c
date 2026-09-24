#include "custom_track_decode.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "decode line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    /* Independently constructed final stored-deflate block containing ABC. */
    unsigned char stream[] = {3,0,0,0,0, 1,3,0,252,255,'A','B','C', 0,0};
    unsigned char out[5] = {0x55,0x55,0x55,0x55,0x55};
    size_t written = 123;
    CHECK(mdkr_custom_track_decode(stream, sizeof(stream), out + 1, 3, &written));
    CHECK(written == 3 && memcmp(out + 1, "ABC", 3) == 0);
    CHECK(out[0] == 0x55 && out[4] == 0x55);
    for (size_t length = 0; length < 13; ++length) {
        CHECK(!mdkr_custom_track_decode(stream, length, out + 1, 3, &written));
        CHECK(written == 0 && out[0] == 0x55 && out[4] == 0x55);
    }
    CHECK(!mdkr_custom_track_decode(stream, sizeof(stream), out + 1, 2, &written));
    stream[0] = 2; /* Actual stream expands past its declared output. */
    CHECK(!mdkr_custom_track_decode(stream, sizeof(stream), out + 1, 3, &written));
    stream[0] = 4; /* Valid deflate ending short of the declaration. */
    CHECK(!mdkr_custom_track_decode(stream, sizeof(stream), out, 5, &written));
    stream[0] = 0;
    CHECK(!mdkr_custom_track_decode(stream, sizeof(stream), out, 5, &written));
    stream[0] = 3;
    stream[9] = 0; /* Stored block LEN/NLEN mismatch. */
    CHECK(!mdkr_custom_track_decode(stream, sizeof(stream), out, 5, &written));
    CHECK(!mdkr_custom_track_decode(NULL, 15, out, 5, &written));
    CHECK(!mdkr_custom_track_decode(stream, 15, NULL, 5, &written));
    CHECK(!mdkr_custom_track_decode(stream, 15, out, 5, NULL));
    CHECK(written == 0);
    puts("custom track bounded decode: PASS");
    return 0;
}
