/**
 * platform.h — Master platform abstraction for the GoldenEye native port.
 *
 * This header replaces the N64 SDK headers (os.h, gu.h, mbi.h, libaudio.h,
 * sptask.h, etc.) when building for PC. It provides stub types, macros,
 * and function declarations that let the original game code compile on
 * modern platforms.
 */
#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

#include <PR/ultratypes.h>
#include <platform_info.h>
#include <platform_stdio.h>

/* Pull in sub-headers.
 * Order matters: GBI first (defines Mtx, Gfx, etc. used by platform_os.h) */
#include "platform_gbi.h"
#include "platform_os.h"
#include "platform_audio.h"

/* mingw-w64's <setjmp.h> has no sigjmp_buf/sigsetjmp/siglongjmp (that trio is
 * a BSD/glibc extension); main_pc.c and gfx_pc.c use it for frame-render
 * crash recovery (GE007_ENABLE_RECOVERY). This mapping exists so those TUs
 * still compile on Windows, but the recovery path itself is forced OFF there
 * (gfx_run_dl gate + crashHandler guard): longjmp out of a Windows signal
 * handler unwinds across the CRT's SEH dispatch frame, which is undefined
 * behavior. Fatal crashes are handled by the SEH unhandled-exception filter
 * in main_pc.c instead. Non-Windows path is untouched -- the system
 * <setjmp.h> already provides the real sigsetjmp/siglongjmp. */
#ifdef _WIN32
#include <setjmp.h>
#define sigjmp_buf jmp_buf
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)
#endif

#endif /* _PLATFORM_H_ */
