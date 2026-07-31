#include <stdint.h>
#include <string.h>

/*
 * Deliberately broken, test-only inputs for check_array_bounds_sweep.py.
 *
 * Production code must be allowed to become sanitizer-clean. These controls
 * prove that each requested UBSan class both compiled and executes, without
 * retaining a known instance of undefined behavior in the game as a sentinel.
 */
int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "array") == 0) {
        volatile int index = 2;
        volatile int values[1] = { 0 };
        return values[index];
    }
    if (strcmp(argv[1], "pointer") == 0) {
        volatile uintptr_t offset = 1;
        volatile char *value = (char *) 0 + offset;
        return value == NULL;
    }
    if (strcmp(argv[1], "shift") == 0) {
        volatile unsigned int count = 32;
        volatile unsigned int value = 1U << count;
        return (int) value;
    }
    return 2;
}
