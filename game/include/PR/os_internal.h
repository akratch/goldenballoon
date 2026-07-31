/*
 * os_internal.h — clean-room umbrella for the internal os_internal_*
 * declaration headers.
 *
 * Pulls in the non-public counterparts of the os.h surface — internal
 * register, exception, TLB, SI, RSP, error, GIO, thread, debug, and host
 * declarations that the OS-level headers build on but that application
 * code isn't meant to include directly. It is a declaration-only
 * compatibility surface: no vendor implementation is present, only the
 * constants and prototypes the rest of the port relies on.
 */

#ifndef _OS_INTERNAL_H_
#define	_OS_INTERNAL_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/os.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

#include "os_internal_reg.h"
#include "os_internal_exception.h"
#include "os_internal_tlb.h"
#include "os_internal_si.h"
#include "os_internal_rsp.h"
#include "os_internal_error.h"
#include "os_internal_gio.h"
#include "os_internal_thread.h"
#include "os_internal_debug.h"
#include "os_internal_host.h"

#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_H */
