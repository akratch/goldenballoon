#include <stdio.h>
#include <string.h>

#include "sha256.h"

static int check(const char *input, const char *expected) {
    char actual[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(input, strlen(input), actual);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "sha256(%s): got %s, expected %s\n", input, actual, expected);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check("", "e3b0c44298fc1c149afbf4c8996fb924"
                          "27ae41e4649b934ca495991b7852b855");
    failures += check("abc", "ba7816bf8f01cfea414140de5dae2223"
                             "b00361a396177a9cb410ff61f20015ad");
    return failures != 0;
}
