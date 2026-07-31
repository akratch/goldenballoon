/*
 * sptask.h — clean-room RSP task descriptor structure and task-type
 * constants.
 *
 * Declares the OSTask layout the CPU fills in to hand a job (audio, graphics,
 * or other microcode) to the RSP, plus the task-flag bits and yield-buffer
 * size constants that go with it. It is a declaration-only compatibility
 * surface: no vendor implementation is present, only the shape of the data
 * this port passes across that boundary.
 */

#ifndef _SPTASK_H_
#define	_SPTASK_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/ultratypes.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Type definitions
 *
 */

/*
 * Task list entry.
 *
 * Everything an application may need to describe a job for the SP.
 * A given task microcode won't necessarily use every field, but the
 * structure covers:
 *
 *	- the task type (audio, gfx, video, ...)
 *	- flag bits, including whether to wait for the DP to drain before
 *	  starting this task (see the "Task Flags field" definitions below)
 *	- boot microcode pointer and size
 *	- microcode pointer and size
 *	- initial DMEM data pointer and size
 *	- DRAM stack pointer and max size
 *	- output buffer pointer, and a pointer to where its length is stored
 *	- a generic data pointer/length pair (e.g. for a display list)
 *	- a pointer/size pair for the buffer that receives saved DMEM state
 *	  when the task yields
 *
 * Caution: field alignment matters here.
 *
 * Caution: the RCP itself writes into the dram_stack, output_buff,
 * output_buff_size, and yield_data_ptr regions, so those buffers need to
 * be cache-line aligned and sized in full 16-byte lines — otherwise a CPU
 * cache writeback can corrupt what the RCP wrote (cache tearing).
 *
 * Caution: every pointer here is a virtual address; translation to
 * physical addresses is handled elsewhere.
 */
typedef struct {
	u32	type;
	u32	flags;

	u64	*ucode_boot;
	u32	ucode_boot_size;

	u64	*ucode;
	u32	ucode_size;

	u64	*ucode_data;
	u32	ucode_data_size;

	u64	*dram_stack;
	u32	dram_stack_size;

	u64	*output_buff;
	u64	*output_buff_size;

	u64	*data_ptr;
	u32	data_size;

	u64	*yield_data_ptr;
	u32	yield_data_size;

} OSTask_t;

typedef union {
    OSTask_t		t;
    long long int	force_structure_alignment;
} OSTask;

typedef u32 OSYieldResult;

#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_ASSEMBLY

/*
 * For the RSP ucode:
 *	offsets into the task structure
 */

#include <PR/sptaskoff.h>

#endif

/*
 * Task Flags field
 */
#define OS_TASK_YIELDED			0x0001
#define OS_TASK_DP_WAIT			0x0002
#define	OS_TASK_LOADABLE		0x0004
#define	OS_TASK_SP_ONLY			0x0008
#define OS_TASK_USR0			0x0010
#define OS_TASK_USR1			0x0020
#define OS_TASK_USR2			0x0040
#define OS_TASK_USR3			0x0080

/*
 * Yield buffer size, in bytes. taskHdrPtr->t.yield_data_ptr must point to
 * a buffer of at least this size, aligned to a 64-bit boundary, and it
 * must be set before the task starts. A task that will never yield can
 * leave this pointer null.
 */
#if	(defined(F3DEX_GBI)||defined(F3DLP_GBI)||defined(F3DEX_GBI_2))
#define	OS_YIELD_DATA_SIZE		0xc00
#elif defined(F3DDKR_GBI)
#define OS_YIELD_DATA_SIZE      0xA00
#else
#define OS_YIELD_DATA_SIZE		0x900
#endif
#define OS_YIELD_AUDIO_SIZE		0x400

/**************************************************************************
 *
 * Global definitions
 *
 */



#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Macro definitions
 *
 */

/*
 * this macro simulates atomic action.
 */
#define	osSpTaskStart(tp)	\
    {				\
        osSpTaskLoad((tp));	\
        osSpTaskStartGo((tp));	\
    }


/**************************************************************************
 *
 * Extern variables
 *
 */


/**************************************************************************
 *
 * Function prototypes
 *
 */

/*
 * break this up into two steps for debugging.
 */
extern void		osSpTaskLoad(OSTask *tp);
extern void		osSpTaskStartGo(OSTask *tp);

extern void		osSpTaskYield(void);
extern OSYieldResult	osSpTaskYielded(OSTask *tp);

#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_SPTASK_H */
