#include "memory.h"

#ifdef __sgi
/**
 * Uses IDO specific -dollar compiler flag to get the current stack pointer.
 * Official Name: diCpuTraceCurrentStack
 */
StackInfo *stack_pointer(void) {
    return (StackInfo *) __$sp;
}
#elif defined(NATIVE_PORT)
/**
 * Native port: MIPS `$sp` inline-asm is unavailable on the host ISA. The value
 * only feeds diagnostic dumps (dump_memory_to_cpak), so the current frame
 * address is a fine stand-in.
 */
StackInfo *stack_pointer(void) {
    return (StackInfo *) __builtin_frame_address(0);
}
#else
/**
 * Uses GCC specific code to get the current stack pointer.
 * Official Name: diCpuTraceCurrentStack
 */
StackInfo *stack_pointer(void) {
    register StackInfo *sp;
    asm volatile("move %0, $sp\n" : "=r"(sp));
    return sp;
}
#endif
