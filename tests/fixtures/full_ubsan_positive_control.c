#include <stdint.h>
#include <string.h>

int main(int argc, char **argv) {
    volatile int32_t negative = -7;
    volatile double too_large = 1.0e300;

    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "shift") == 0) {
        return negative << 3;
    }
    if (strcmp(argv[1], "float-cast") == 0) {
        return (int32_t) too_large;
    }
    return 2;
}
