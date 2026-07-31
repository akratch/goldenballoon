
/*
 * ultra64.h — clean-room single top-level compatibility umbrella the
 * decompiled game code includes.
 *
 * Pulls together every PR/ declaration header this port needs — scalar
 * types, RCP registers, the OS surface, the region allocator, RSP task
 * descriptors, audio, GBI/microcode, and error/logging support — into the
 * one include the decompiled game sources expect. It is a declaration-only
 * compatibility surface: no vendor implementation is present, only the
 * constants and prototypes the rest of the port relies on.
 */

#ifndef _ULTRA64_H_
#define _ULTRA64_H_

#include <PR/ultratypes.h>
#include <PR/rcp.h>
#include <PR/os.h>
#include <PR/region.h>
#include <PR/sptask.h>
#include <PR/mbi.h>
#include <PR/libaudio.h>
#include <PR/gu.h>
#include <PR/ucode.h>
#include <PR/ultraerror.h>
#include <PR/ultralog.h>

#endif
