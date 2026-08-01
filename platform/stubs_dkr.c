/**
 * stubs_dkr.c — libultra (OS/scheduler/VI/PI/controller/audio-io) replacement
 * for the mdkr64 native port.
 *
 * DKR keeps its full <PR/*.h> SDK headers (they DECLARE the API); this file
 * DEFINES it against the single-threaded cooperative model from PLAN.md:
 *   - threads are no-ops; the "threads" collapse into main_pc.c's call chain,
 *   - message queues are ring buffers,
 *   - the graphics scheduler dispatches a submitted gfx task synchronously
 *     (run the HLE, immediately post its done-message),
 *   - a blocking osRecvMesg on an empty scheduler-client (video) queue is the
 *     frame boundary: it calls platform_frame_sync() and posts one retrace.
 *   - osPiStartDma = memcpy from the ROM image + instant DMA-done message.
 */
#include <ultra64.h>
#include <PR/sched.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, for the MDKR_GRIDMASK A/B hook */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include "user_paths.h"
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <direct.h>    /* _mkdir  (Win32 has no mode argument) */
#include <io.h>        /* _commit, _fileno */
#include <malloc.h>    /* _aligned_malloc / _aligned_free */
#include <windows.h>   /* MoveFileExA */
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

/* The N64 SDK's OSContPad has a field literally named `errno`. Host libc makes
 * errno a function-like storage macro, so retain access through this helper and
 * then remove the macro before any controller-field expressions are parsed. */
static int dkr_host_errno(void) {
    return errno;
}
#undef errno

/* ======================================================================== *
 *  Durable-write primitives (POSIX / Win32 / Emscripten)
 * ------------------------------------------------------------------------
 *  Every save path below follows the same transaction: write a temp file,
 *  flush it to stable storage, atomically replace the real file, then flush
 *  the directory entry. Windows implements two of those three differently, so
 *  the mapping is stated once here instead of at each of the three call sites.
 *
 *    fsync(fileno(f))  ->  _commit(_fileno(f))            same guarantee
 *    rename(a, b)      ->  MoveFileExA(MOVEFILE_REPLACE_EXISTING |
 *                                      MOVEFILE_WRITE_THROUGH)
 *        NOT rename(): the Windows CRT's rename() FAILS when the destination
 *        already exists, so a plain rename would leave every save after the
 *        first one silently unwritten. WRITE_THROUGH commits the replacement
 *        before returning.
 *    fsync(dirfd)      ->  no-op. Win32 cannot open a directory as a file
 *        descriptor, and MOVEFILE_WRITE_THROUGH already gives the ordering
 *        the directory flush exists to provide.
 *    mkdir(p, 0700)    ->  _mkdir(p). Win32 directories inherit their ACL from
 *        the parent; there is no mode argument. The save directory lives under
 *        the user's own profile, so the inherited ACL is already user-private.
 *
 *  All three return 0 on success and non-zero on failure, like their POSIX
 *  models, so the call sites read identically on every platform.
 * ======================================================================== */
static int dkr_fs_sync_file(FILE *file) {
#if defined(__EMSCRIPTEN__)
    (void) file;
    return 0;                       /* MEMFS; durability is IDBFS's job */
#elif defined(_WIN32)
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

static int dkr_fs_sync_dir(const char *path) {
#if defined(__EMSCRIPTEN__) || defined(_WIN32)
    (void) path;
    return 0;
#else
    int failed = 0;
    int directory_fd = open(path, O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0 || fsync(directory_fd) != 0) {
        failed = 1;
    }
    if (directory_fd >= 0 && close(directory_fd) != 0) {
        failed = 1;
    }
    return failed;
#endif
}

static int dkr_fs_replace(const char *from, const char *to) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return MoveFileExA(from, to,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0
               : -1;
#else
    return rename(from, to);
#endif
}

static int dkr_fs_mkdir_private(const char *path) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

/*
 * An EEPROM write is not complete on the web until IDBFS has committed the
 * corresponding MEMFS image. EM_ASYNC_JS participates in the build's Asyncify
 * unwind, so the game cannot advance past a save point (or expose a later,
 * partially persisted generation) while that transaction is still pending.
 */
EM_ASYNC_JS(int, mdkr_persist_save_async, (int kind), {
    try {
        if (typeof Module.__mdkrPersist === "function") {
            await Module.__mdkrPersist({
                reason: kind ? "controller-pak" : "eeprom", urgent: true
            });
            return 1;
        }
        if (typeof FS === "undefined") return 0;
        await new Promise((resolve, reject) => {
            FS.syncfs(false, (error) => error ? reject(error) : resolve());
        });
        return 1;
    } catch (error) {
        if (typeof Module.__mdkrPersistFailed === "function") {
            Module.__mdkrPersistFailed(String(
                error && error.message ? error.message : error));
        }
        return 0;
    }
});
#endif

#include "platform_os.h"
#include "pacing_policy.h"
#include "present_sched.h"
#include "presentation_snapshot.h"
#include "gameplay_event_trace.h"
#include "input_consumption_trace.h"
#include "audi_port_dkr.h"
#include "mdkr_bounds.h"
#include "gfx_ptr.h"     /* gfx_ptr_store — register non-arena DL pointers */
#include "fast3d/gfx_shadow_frame.h"
#include "fast3d/gfx_pc_dkr.h"
#include "address_domains.h"
#include "save_codec.h"
#include "virtual_pak.h"
#ifdef MDKR_WEBGPU_BACKEND
#include "fast3d/gfx_webgpu.h"
#endif

/* Renderer front-end (platform/fast3d/gfx_pc_dkr.c, F3DDKR HLE). gfx_run and
 * the frame bracket are declared by gfx_pc.h via gfx_pc_dkr.h. */
extern bool gfx_renderer_failed(void);

static bool renderer_recovery_deferred(void) {
#if defined(MDKR_WEBGPU_BACKEND) && !defined(__EMSCRIPTEN__)
    return mdkr_render_backend() == MDKR_BACKEND_WEBGPU &&
           gfx_webgpu_runtime_recovery_pending();
#else
    return false;
#endif
}

/* game/src/camera.c (NATIVE_PORT): each viewport's view-projection rebuilt from
 * the INTERPOLATED camera inputs of the published snapshot pair (spec §7). */
extern size_t mdkr_camera_interpolated_view_projections(
    uint64_t numerator, uint64_t denominator,
    GfxShadowReplayViewProjection *out, size_t capacity);

/* ======================================================================== *
 *  libultra runtime globals
 * ======================================================================== */
s32   osTvType   = 1;            /* 1 = NTSC (us.v80) */
void *osRomBase  = 0;
s32   osResetType = 0;
s32   osCicId    = 6103;         /* DKR uses CIC-6103 */
s32   osVersion  = 0;
u32   osMemSize  = 4 * 1024 * 1024;
s32   osAppNMIBuffer[16];
u64   osClockRate = 62500000ULL;

void osInitialize(void) {
    osTvType = platform_source_tv_type();
    osMemSize = 4 * 1024 * 1024;
}
void __osInitialize_common(void) {}

/* ======================================================================== *
 *  RDRAM stand-in arena + 32-bit pointer reconstruction
 * ======================================================================== */
void     *g_dkrArenaBase = NULL;
uintptr_t g_dkrArenaHi   = 0;
uint32_t  g_dkrArenaSize = 0;

void *dkr_arena_init(uint32_t size) {
    /* Align the block to its own size so [base, base+size) never straddles a
     * 4 GB boundary — then every arena pointer shares one high-32-bit value. */
    uintptr_t align = size;
#if UINTPTR_MAX == UINT32_MAX
    /* ILP32: pointers are 32-bit and the arena's high bits are 0, so an
     * arena address is its own low-32. On LP64 the arena sits at a high 64-bit
     * address (e.g. 0xc16000000) whose low-32 never collides with DKR's N64
     * segment tokens (0x0N000000, segments 0-15 => 0x00000000..0x0FFFFFFF). On
     * ILP32 a low arena (e.g. 0x02000000) DOES collide: dkr_resolve's arena
     * reconstruction would swallow the framebuffer/zbuffer segment token
     * 0x0N000000 (SETCIMG->NULL, the M3b white-z-clear-FILLRECT symptom). Force
     * the arena ABOVE the 256 MB segment ceiling so segment tokens always fall
     * through to the segment table and arena addresses (>= 0x10000000) never look
     * like a segment token — no change to the shared resolver. */
    if (align < 0x10000000u) align = 0x10000000u;
#endif
    void *p = NULL;
#ifdef _WIN32
    /* The Windows CRT has neither aligned_alloc (C11) nor posix_memalign;
     * _aligned_malloc is the only over-aligned allocator, it takes (size,
     * align) in the opposite order, and its blocks MUST be released with
     * _aligned_free — see dkr_arena_shutdown. */
    p = _aligned_malloc(size, align);
#else
#if defined(_ISOC11_SOURCE) || __STDC_VERSION__ >= 201112L
    /* aligned_alloc requires size to be a multiple of alignment; only use it when
     * align == size (native). Otherwise (web, align > size) use posix_memalign. */
    if (align == (uintptr_t)size) {
        p = aligned_alloc(align, size);
    }
#endif
    if (!p) {
        if (posix_memalign(&p, align, size) != 0) p = NULL;
    }
#endif
    if (!p) {
        fprintf(stderr, "[MEM] arena alloc of %u bytes failed\n", size);
        abort();
    }
    memset(p, 0, size);
    g_dkrArenaBase = p;
    g_dkrArenaSize = size;
    g_dkrArenaHi   = (uintptr_t)p & ~(uintptr_t)0xFFFFFFFFu;
    printf("[MEM] arena %u bytes @ %p (hi=0x%llx)\n",
           size, p, (unsigned long long)g_dkrArenaHi);
    return p;
}

void dkr_arena_shutdown(void) {
    if (g_dkrArenaBase != NULL) {
#ifdef _WIN32
        _aligned_free(g_dkrArenaBase);   /* must match _aligned_malloc above */
#else
        free(g_dkrArenaBase);
#endif
    }
    g_dkrArenaBase = NULL;
    g_dkrArenaHi = 0;
    g_dkrArenaSize = 0;
}

void *dkr_lo32_to_ptr(uint32_t lo32) {
    if (lo32 == 0) return NULL;
    return (void *)(g_dkrArenaHi | (uintptr_t)lo32);
}

/* OS_K0_TO_PHYSICAL on LP64 (NATIVE_PORT): the game feeds nearly every display
 * list pointer through this. The returned token is (u32)(hostptr - 0x80000000);
 * the F3DDKR resolver (dkr_resolve) reconstructs ARENA pointers arithmetically
 * (arena is size-aligned), so those need no bookkeeping. But NON-arena pointers
 * — game globals / rodata static display lists such as gViewportStack and
 * dMenuHudDrawModes — lose their host high bits on truncation and can only be
 * recovered from the pointer registry, keyed on (u32)hostptr. Register exactly
 * those here so dkr_resolve's registry fallback can hand the full pointer back.
 * (PLAN decision 3: the store side owns registration at the conversion boundary.) */
u32 dkr_k0_to_physical(const void *x) {
    uintptr_t p = (uintptr_t)x;
    uintptr_t base = (uintptr_t)g_dkrArenaBase;
    int in_arena = (p >= base && p < base + (uintptr_t)g_dkrArenaSize);
    /* Register ONLY genuine 64-bit non-arena host pointers (globals / rodata).
     * Two kinds of argument must be EXCLUDED or the registry gets poisoned:
     *   - arena pointers (in_arena): reconstructable arithmetically, and the
     *     registry is deliberately reserved for the non-reconstructable ones.
     *   - already-truncated dkrptr32 TOKENS (value <= 0xFFFFFFFF): DKR frequently
     *     feeds OS_K0_TO_PHYSICAL a stored token, e.g. TextureHeader.cmd. Storing
     *     (void*)token as if it were a host pointer makes dkr_resolve hand that
     *     bogus low value back (registry beats arena reconstruction), so the
     *     texture command list never executes and the sprite renders untextured. */
    /* Register genuine 64-bit non-arena host pointers (LP64 globals/rodata). On
     * ILP32 pointers are 32-bit and globals also reach DLs via `(s32)ptr+K0BASE`
     * / raw casts that never pass through here, so registration can't cover them;
     * dkr_resolve recovers ILP32 host pointers DIRECTLY from the token instead.
     * This registration stays LP64-only. */
#if UINTPTR_MAX > UINT32_MAX
    if (p > 0xFFFFFFFFu && !in_arena) {
        gfx_ptr_store_persistent(x);
    }
#else
    (void)in_arena;
#endif
    return (u32)(p - 0x80000000u);
}

/* Register a RAW host pointer that is about to be truncated into a display-list
 * word (gDma1p: gSPDisplayList/gSPBranchList to static rodata DLs, etc.).
 * Genuine non-arena LP64 pointers receive a stage epoch so an already-queued
 * task survives one teardown; arena pointers reconstruct arithmetically and
 * pre-computed 32-bit tokens resolve through the segment table. Without the
 * registration, a global DL pointer whose low 32 bits carry a live segment
 * nibble can false-resolve into unrelated arena memory. */
void dkr_dl_register_host_ptr(const void *x) {
    /* LP64-only (see dkr_k0_to_physical): ILP32 recovers host pointers directly
     * in dkr_resolve, so no registration is needed there. */
    uintptr_t p = (uintptr_t)x;
    uintptr_t base = (uintptr_t)g_dkrArenaBase;
    int in_arena = p >= base && p < base + (uintptr_t)g_dkrArenaSize;
#if UINTPTR_MAX > UINT32_MAX
    if (p > 0xFFFFFFFFu && !in_arena) {
        gfx_ptr_store_persistent(x);
    }
#else
    (void)in_arena;
#endif
}

/* ======================================================================== *
 *  Threads — no-ops (single-threaded cooperative model)
 * ======================================================================== */
void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg,
                    void *sp, OSPri pri) {
    (void)entry; (void)arg; (void)sp;
    if (t) { t->id = id; t->priority = pri; t->state = OS_STATE_STOPPED; }
}
void osStartThread(OSThread *t)          { if (t) t->state = OS_STATE_RUNNABLE; }
void osStopThread(OSThread *t)           { if (t) t->state = OS_STATE_STOPPED; }
void osDestroyThread(OSThread *t)        { (void)t; }
void osSetThreadPri(OSThread *t, OSPri p){ if (t) t->priority = p; }
OSPri osGetThreadPri(OSThread *t)        { return t ? t->priority : 0; }
OSId  osGetThreadId(OSThread *t)         { return t ? t->id : 0; }

/* Active-thread queue: a single sentinel with priority -1 terminates every
 * `while (node->priority != -1)` walk in thread0_epc.c immediately. */
static OSThread s_activeSentinel = { 0 };
OSThread *__osGetActiveQueue(void) {
    s_activeSentinel.priority = -1;
    s_activeSentinel.tlnext   = &s_activeSentinel;
    return &s_activeSentinel;
}

u32  __osDisableInt(void)      { return 0; }
void __osRestoreInt(u32 f)     { (void)f; }
OSIntMask osGetIntMask(void)   { return 0; }
OSIntMask osSetIntMask(OSIntMask m) { (void)m; return 0; }

/* ======================================================================== *
 *  Message queues (ring buffers) + frame pacing
 * ======================================================================== */
void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count) {
    if (!mq) return;
    memset(mq, 0, sizeof(*mq));
    mq->msg = msg;
    mq->msgCount = count;
}

static s32 mq_enqueue(OSMesgQueue *mq, OSMesg m) {
    if (mq && mq->msg && mq->validCount < mq->msgCount) {
        s32 idx = (mq->first + mq->validCount) % mq->msgCount;
        mq->msg[idx] = m;
        mq->validCount++;
        return 0;
    }
    return -1;
}
static s32 mq_dequeue(OSMesgQueue *mq, OSMesg *out) {
    if (mq && mq->validCount > 0) {
        if (out) *out = mq->msg[mq->first];
        mq->first = (mq->first + 1) % mq->msgCount;
        mq->validCount--;
        return 0;
    }
    return -1;
}

/* Set when osCreateScheduler runs; the queue DKR pushes gfx tasks onto. */
static OSMesgQueue *s_schedInterruptQ = NULL;

/* The scheduler's VIDEO client queue (captured in osScAddClient). A blocking
 * recv on it is the frame boundary; its retrace messages are synthesised from
 * a wall-clock field budget so fb_update measures the real elapsed-field count.
 * See platform_vi_pace_measure() (platform_sdl_min.c). */
static OSMesgQueue *s_videoClientQueue = NULL;
/* Real display-list walks observed at the previous branch entry — the host
 * subloop's completed-authored-image test (gfx_dkr_real_walk_count). */
static uint64_t     s_lastRealWalkCount = 0;
static int          s_testDelayedEndpointReplay = -1;
static s32          s_viFieldsPending  = 0;   /* retrace fields available to drain */
/* A host suspension is observed at a presentation/pacing opportunity, but
 * audio time is credited only when the resulting fixed game ticket completes.
 * Carry the rebase to that ticket exactly once. */
static bool         s_audioRebasePending = false;

static bool test_delayed_endpoint_replay(void) {
    if (s_testDelayedEndpointReplay < 0) {
        const char *value = getenv("MDKR_TEST_DELAYED_ENDPOINT_REPLAY");
        s_testDelayedEndpointReplay =
            present_sched_internal_replay_test_enabled() &&
            value != NULL && value[0] == '1' ? 1 : 0;
    }
    return s_testDelayedEndpointReplay != 0;
}

/* Gfx-task done payload. gfxtask_wait() reads received[1] as the status word
 * (0 == OSMESG_SWAP_BUFFER). */
static OSMesg s_gfxDone[2] = { (OSMesg)(intptr_t)OS_SC_DONE_MSG, (OSMesg)0 };

/* Mirror of DKR_OSTask (rcp_dkr.h) for the fields the dispatcher reads. */
typedef struct {
    void *next; u32 state; u32 flags; void *frameBuffer;
    OSTask_t task;
    OSMesgQueue *mesgQueue; OSMesg mesg;
    u64 presentationAuthoredTick;
} ShimScTask;

static void sched_dispatch_task(OSMesg msg) {
    ShimScTask *t = (ShimScTask *)msg;
    if (!t) return;
    if (t->task.type == M_GFXTASK) {
        /* One M_GFXTASK == one frame's display list (DKR submits a single gfx
         * task per frame). Bracket the interpret with start/end frame so the
         * F3DDKR HLE resets per-frame state, walks the DL, and flushes+presents
         * through the backend. data_ptr is the DL head pointer. */
        MDKR_TRACE("gfxtask: type=%u dl=%p len=%u", (unsigned)t->task.type,
                   (void *)t->task.data_ptr, (unsigned)t->task.data_size);
        if (present_sched_render_elided()) {
            /* A minimized/hidden native window has no useful sink. Complete
             * the emulated RSP task without walking or submitting its display
             * list; input, audio and fixed simulation keep running at the
             * throttled host boundary. Resume rebases retained history before
             * the next task is allowed through. */
            present_sched_note_render_elided();
            goto task_complete;
        }
        /* A transiently saturated backend has not opened a frame transaction.
         * Complete the emulated task without walking into a null encoder; a
         * fatal refusal continues through the existing renderer-failure path. */
        if (!gfx_start_frame(t->presentationAuthoredTick) &&
            !gfx_renderer_failed()) {
            goto task_complete;
        }
        if (gfx_renderer_failed()) {
            if (renderer_recovery_deferred()) {
                fprintf(stderr,
                        "[webgpu] renderer failure latched; deferring recovery "
                        "to the complete-frame boundary\n");
                goto task_complete;
            }
            fprintf(stderr,
                    "[mdkr64] renderer entered an unrecoverable state; "
                    "stopping before game state advances.\n");
            fflush(stderr);
            platform_request_exit(EXIT_FAILURE);
            goto task_complete;
        }
        gfx_run((void *)t->task.data_ptr);
        gfx_end_frame();
        /*
         * Zero-delta replay harness (Phase 3 Wave B slice 1). Re-walk the list
         * this tick just consumed, with the frozen matrix registry restored and
         * the SAME view-projection, and overdraw the result. It must produce a
         * byte-identical image and must not perturb a single authoritative bit
         * — that is the whole assertion, and it is what proves the replay
         * machinery is sound before slice 2 starts feeding it a moved camera.
         * Test seam only; never armed on a shipping path.
         */
        if (present_sched_test_replay_walk()) {
            (void)gfx_dkr_replay_walk(NULL, 0);
        }

        if (gfx_renderer_failed()) {
            if (renderer_recovery_deferred()) {
                fprintf(stderr,
                        "[webgpu] renderer failure latched; deferring recovery "
                        "to the complete-frame boundary\n");
                goto task_complete;
            }
            fprintf(stderr,
                    "[mdkr64] renderer entered an unrecoverable state; "
                    "stopping at the frame boundary.\n");
            fflush(stderr);
            platform_request_exit(EXIT_FAILURE);
            goto task_complete;
        }
    } else {
        MDKR_TRACE("sched task type=%u (non-gfx)", (unsigned)t->task.type);
    }
task_complete:
    if (t->task.type == M_GFXTASK) {
        /* gfx_end_frame normally clears this frame's host-only projected
         * shadow markers. Also clear them on a start-frame failure, where the
         * HLE deliberately never reaches gfx_end_frame. */
        gfx_shadow_projected_ranges_reset();
    }
    /* Signal task completion so gfxtask_wait()/audio reply recv unblocks. */
    if (t->mesgQueue) {
        mq_enqueue(t->mesgQueue, (OSMesg)s_gfxDone);
    }
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    (void)flags;
    if (mq && mq == s_schedInterruptQ) {
        uintptr_t task_address = (uintptr_t)msg;
        /*
         * Ordinary libultra queues legitimately carry small integer tokens
         * (the SI completion message is 1). If a caller passes the wrong queue,
         * never reinterpret such a token as a scheduler task and dereference
         * near NULL. Real ShimScTask objects satisfy pointer alignment.
         */
        if (task_address < 4096u ||
            task_address % _Alignof(ShimScTask) != 0u) {
            fprintf(
                stderr,
                "[scheduler] rejected invalid task message %p\n",
                (void *)msg);
            return -1;
        }
        /* A submitted RSP task — run it now (see PLAN §1 collapse). */
        sched_dispatch_task(msg);
        return 0;
    }
    return mq_enqueue(mq, msg);
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    (void)flags;
    if (mq && mq->msg && mq->validCount < mq->msgCount) {
        mq->first = (mq->first - 1 + mq->msgCount) % mq->msgCount;
        mq->msg[mq->first] = msg;
        mq->validCount++;
        return 0;
    }
    return -1;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flags) {
    /* Real queued messages take priority (non-video queues; any stray). */
    if (mq && mq->validCount > 0) {
        return mq_dequeue(mq, msg);
    }

    /* ---- VIDEO client queue: authoritative fixed-ticket adapter ----------
     * A blocking recv on an empty queue is the host boundary. Host elapsed time
     * advances HostFrameDriver; one fixed ticket is translated back into the
     * retrace messages fb_update expects. Multi-tick lateness stays in driver
     * debt and causes repeated authored updates, never updateRate 4/6. */
    if (mq && mq == s_videoClientQueue) {
        if (s_viFieldsPending <= 0) {
            if (flags == OS_MESG_NOBLOCK) {
                return -1;               /* no more fixed-ticket notifications */
            }
            unsigned ticks_due;
            unsigned trace_fields = 0;
            int oracle_update_fields = 0;
            const bool oracle_variable_ticket =
                platform_oracle_update_fields(
                    (uint64_t)g_simTickCounter, &oracle_update_fields) != 0;
            const uint64_t perf_entry = present_perf_now();
            const int subloop = platform_present_subloop_fields();
            const bool catchup_ticket =
                present_sched_pending_ticks() != 0u;
            if (oracle_variable_ticket) {
                /* Diagnostic only: replay one complete game pass at the exact
                 * updateRate observed from the real ROM. Shipping never enters
                 * this branch and retains exact fixed tickets under lateness. */
                trace_fields = (unsigned)oracle_update_fields;
                g_viLastWallFields = oracle_update_fields;
                ticks_due = 1u;
            } else if (!subloop && !catchup_ticket) {
                /*
                 * Pace one host opportunity and let the authoritative driver
                 * turn its elapsed fields into fixed-size tick tickets.
                 */
                (void)platform_vi_pace_measure();
                trace_fields = (unsigned)g_viLastWallFields;
                {
                    const bool rebased = platform_vi_pace_rebased() != 0;
                    if (rebased) {
                        s_audioRebasePending = true;
                    }
                    ticks_due = present_sched_advance_fields(
                        trace_fields, rebased);
                }
            } else if (!subloop) {
                /* Ordinary late-frame debt is drained as repeated authored
                 * ticks. Do not sample/sleep again until that debt is gone. */
                g_viLastWallFields = 0;
                ticks_due = 0;
            } else if (!catchup_ticket) {
                /*
                 * Presentation subloop engaged (design §1). Pacing moves AFTER
                 * the tick's own present, below, so this entry only needs the
                 * tick's field total, which the subloop accumulates. Zero here
                 * is a placeholder the subloop replaces.
                 */
                ticks_due = 0;
            } else {
                /* A prior host opportunity issued more than one ticket. The
                 * correctness-first catch-up path builds/presents this tick,
                 * but does not wait for another presentation interval. */
                ticks_due = 0;
                g_viLastWallFields = 0;
            }
            /* The synthetic N64 COUNTER advances once per completed fixed
             * game pass, never once per present and never by an oversleep's
             * lumped wall-field count. */
            platform_vi_tick_clock_commit(
                oracle_variable_ticket
                    ? (unsigned)oracle_update_fields
                    : present_sched_tick_fields());
            g_viLastFields = platform_vi_pace_compensating()
                ? (oracle_variable_ticket
                       ? (s32)oracle_update_fields
                       : (s32)present_sched_tick_fields())
                : 1;
            /* Fidelity streams: one row per complete authoritative game pass.
             * Both are env-gated no-ops in ordinary runs. */
            {
                extern void mdkr_sim_hash_frame(void);
                extern void mdkr_test_render_tick_advance(void);
                mdkr_sim_hash_frame();
                gameplay_event_trace_tick((uint64_t)g_simTickCounter);
                input_consumption_trace_tick((uint64_t)g_simTickCounter);
                /* Presentation snapshot (spec §7, Phase 3 Wave A): the same
                 * authoritative tick boundary, immediately after the hash so
                 * the hash can never observe it. Read-only over live state
                 * and env-gated by MDKR_PRESENT_SNAPSHOT; a no-op otherwise. */
                {
                    const uint64_t perf_snapshot = present_perf_now();
                    presentation_snapshot_capture(
                        (uint64_t)g_simTickCounter);
                    present_perf_add(PRESENT_PERF_SNAPSHOT, perf_snapshot);
                }
                /* Purity-gate parity: one advance per authoritative tick,
                 * AFTER this tick's render ran (or was skipped), so
                 * render_scene sees a stable parity for the whole tick. */
                mdkr_test_render_tick_advance();
            }
            /*
             * The real walk is already the exact alpha-zero image for its
             * authored task. Expose that completed image directly: replaying
             * it here would happen after the game has built the next list, so
             * any dependency not explicitly retained could have changed even
             * though the command stream itself is still valid. Production
             * holds the endpoint below; midpoint walks exist only under the
             * explicit internal replay seam.
             *
             * MDKR_TEST_DELAYED_ENDPOINT_REPLAY plus the versioned internal
             * token restores the old redraw only as a negative control.
             */
            const uint64_t walks = gfx_dkr_real_walk_count();
            const bool dl_fresh = walks != s_lastRealWalkCount;
            s_lastRealWalkCount = walks;
            bool endpoint_drew = false;
            if (!oracle_variable_ticket && subloop && !catchup_ticket &&
                !present_sched_render_elided() && dl_fresh) {
                const uint64_t authored_tick =
                    gfx_dkr_last_walked_authored_tick();
                GfxShadowReplayViewProjection endpoint_views[4] = { 0 };
                size_t endpoint_count =
                    mdkr_camera_interpolated_view_projections(
                        0u, 1u, endpoint_views,
                        sizeof(endpoint_views) / sizeof(endpoint_views[0]));
                gfx_shadow_camera_endpoint_validate(
                    endpoint_views, endpoint_count);
                if (test_delayed_endpoint_replay() &&
                    present_sched_smoothing_enabled()) {
                    endpoint_drew = gfx_dkr_replay_walk_interpolated(
                        endpoint_views, endpoint_count, 0u, 1u);
                } else {
                    endpoint_drew = true;
                }
                if (endpoint_drew) {
                    present_sched_note_endpoint(
                        test_delayed_endpoint_replay(), authored_tick);
                }
            }
            if (present_sched_render_elided()) {
                platform_frame_service();
            } else {
                const uint64_t perf_present = present_perf_now();
                if (!subloop || endpoint_drew) {
                    platform_frame_sync();                  /* present this frame */
                } else {
                    /* No graphics task completed for this opportunity. Hold
                     * the last complete image instead of swapping undefined
                     * back-buffer contents. */
                    present_sched_note_stale();
                    platform_frame_sync_no_swap();
                }
                present_perf_add(PRESENT_PERF_PRESENT, perf_present);
            }
            if (!oracle_variable_ticket && subloop && !catchup_ticket) {
                /*
                 * THE HOST-OPPORTUNITY SUBLOOP.
                 *
                 * The exact real-walk endpoint just went out at alpha 0. Now
                 * burn wall
                 * time one present-quantum at a time
                 * until the next authoritative tick is due, and while it is
                 * not, hold the completed authored surface image. Production
                 * 1.0.1 never redraws here. Explicit internal replay seams can
                 * exercise an interpolated walk of the same display list.
                 *
                 * Pacing after the tick endpoint keeps the internal diagnostic
                 * alpha monotonic (0, then 1/2, then the next tick's 0) without
                 * changing production surface-update cadence.
                 *
                 * The instruments above (state hash, presentation snapshot,
                 * render-tick advance) stay exactly where they are: once per
                 * branch entry, which is once per authoritative tick, never in
                 * this loop (design R8). Nothing in here touches game state —
                 * gfx_dkr_replay_walk re-walks an already-built list at the HLE
                 * layer and never re-enters game/src (design R3).
                 */
                uint64_t wall_total_units = 0u;
                for (;;) {
                    uint64_t units = platform_vi_present_pace_units();
                    if (UINT64_MAX - wall_total_units < units) {
                        wall_total_units = UINT64_MAX;
                    } else {
                        wall_total_units += units;
                    }
                    {
                        const bool rebased = platform_vi_pace_rebased() != 0;
                        if (rebased) {
                            s_audioRebasePending = true;
                        }
                        ticks_due = present_sched_advance_units(
                            units, rebased);
                    }
                    if (ticks_due != 0 || platform_exit_requested()) {
                        break;
                    }
                    bool drew = false;
                    if (!present_sched_render_elided() && dl_fresh &&
                        present_sched_smoothing_enabled() &&
                        !test_delayed_endpoint_replay()) {
                        GfxShadowReplayViewProjection views[4];
                        uint64_t numerator = 0;
                        uint64_t denominator = 1;
                        size_t count;
                        const uint64_t perf_interp = present_perf_now();
                        present_sched_alpha(&numerator, &denominator);
                        count = mdkr_camera_interpolated_view_projections(
                            numerator, denominator, views,
                            sizeof(views) / sizeof(views[0]));
                        present_perf_add(PRESENT_PERF_INTERP, perf_interp);
                        /*
                         * Count the interpolated present only if the walk
                         * actually produced an image. gfx_dkr_replay_walk
                         * refuses whenever the frozen registry, the held list
                         * or the walk-entry state is unavailable, and an
                         * unconditional note made `interp` report presents that
                         * drew nothing -- exactly the case a gate reading that
                         * counter is trying to distinguish.
                         */
                        {
                            const uint64_t perf_replay = present_perf_now();
                            drew = gfx_dkr_replay_walk_interpolated(
                                views, count, numerator, denominator);
                            present_perf_add(PRESENT_PERF_REPLAY, perf_replay);
                        }
                        if (drew) {
                            present_sched_note_interpolated((unsigned)count);
                        } else {
                            present_sched_note_stale();
                        }
                    } else {
                        /* Production, smoothing-off, or a stale list: retain
                         * the front/surface image without walking memory the
                         * game may be overwriting. */
                        present_sched_note_stale();
                    }
                    /*
                     * Nothing new was drawn on the else branches, so DO NOT
                     * swap: the back buffer's contents after the tick's own
                     * swap are undefined, and presenting it shows garbage
                     * rather than holding the tick's image (see
                     * platform_frame_sync_no_swap). Everything else about the
                     * frame boundary -- input pump, frame counter, frame dump,
                     * traces -- still runs, and the present was already paced
                     * at the top of this loop, so this cannot busy-spin and
                     * still yields to rAF in the browser.
                     */
                    {
                        const uint64_t perf_ipresent = present_perf_now();
                        if (present_sched_render_elided()) {
                            platform_frame_service();
                        } else if (drew) {
                            platform_frame_sync();
                        } else {
                            platform_frame_sync_no_swap();
                        }
                        present_perf_add(PRESENT_PERF_IPRESENT, perf_ipresent);
                    }
                }
                /* wall_total is host-time telemetry only. Ticket width is
                 * published below and cannot depend on present count. */
                trace_fields = (unsigned)(
                    wall_total_units / UINT64_C(1000000000));
            }
            if (oracle_variable_ticket) {
                platform_input_commit_tick((uint64_t)g_simTickCounter + 1u);
            } else if (present_sched_take_tick()) {
                platform_input_commit_tick(present_sched_issued_ticks());
            } else if (!platform_exit_requested()) {
                fprintf(stderr,
                        "[scheduler] host boundary produced no fixed-step "
                        "ticket; stopping before an unscheduled game pass\n");
                platform_request_exit(EXIT_FAILURE);
            }
            /* fb_update consumes one authored ticket. Late time remains
             * in the driver's bounded debt queue and becomes another game
             * pass, never one updateRate=4/6 mega-step. The compensation-off
             * diagnostic retains its deliberate one-field lie. */
            /*
             * The tick index advances only after every present belonging to
             * this tick. Scripted input uses that index, while live host edges
             * are captured per opportunity and committed above only once per
             * issued ticket. Catch-up service and extra presents cannot consume
             * input themselves.
             */
            g_simTickCounter++;
            platform_headless_tick_complete(g_simTickCounter);
            present_sched_trace_entry(trace_fields, ticks_due,
                                      g_frameCounter);
            /* Credit exactly this completed ticket, then service at most one
             * independently due audio quantum AFTER the ordered game tick that
             * queued its music/SFX events. Presentation rate cannot change PCM
             * timing. Grouped lateness leaves one quantum per catch-up game
             * pass, and a host suspension rebase is consumed exactly once by
             * the first ticket produced after it. */
            dkr_audio_advance_fields(
                oracle_variable_ticket
                    ? (unsigned)oracle_update_fields
                    : present_sched_tick_fields(),
                s_audioRebasePending);
            s_audioRebasePending = false;
            if (!platform_exit_requested()) {
                dkr_audio_service_tick();
            }
            /* Exactly one compatibility notification per fixed ticket. The
             * measured bootstrap logical-delta phase lives in video.c and
             * cannot drain a second simulation ticket. */
            s_viFieldsPending = mdkr_pacing_queue_refill(
                s_viFieldsPending, 1, 8);
            present_perf_add(PRESENT_PERF_TICKWALL, perf_entry);
        }
        s_viFieldsPending--;
        if (msg) *msg = (OSMesg)(intptr_t)OS_SC_RETRACE_MSG;
        return 0;
    }

    if (flags == OS_MESG_NOBLOCK) {
        return -1;
    }
    /* Non-video blocking recv on an empty queue — degenerate (not hit on the
     * DKR boot/gameplay path). Preserve the legacy behaviour: advance a frame. */
    platform_frame_sync();
    mq_enqueue(mq, (OSMesg)(intptr_t)OS_SC_RETRACE_MSG);
    return mq_dequeue(mq, msg);
}

/* ======================================================================== *
 *  Scheduler (cooperative)
 * ======================================================================== */
void osCreateScheduler(OSSched *s, void *stack, OSPri pri, u8 mode, u8 numFields) {
    (void)stack; (void)pri; (void)mode; (void)numFields;
    if (!s) return;
    osCreateMesgQueue(&s->interruptQ, s->intBuf, OS_SC_MAX_MESGS);
    osCreateMesgQueue(&s->cmdQ, s->cmdMsgBuf, OS_SC_MAX_MESGS);
    s->clientList = NULL;
    s->curRSPTask = NULL;
    s->curRDPTask = NULL;
    s->frameCount = 0;
    s->doAudio = 0;
    s_schedInterruptQ = &s->interruptQ;
}
void osScAddClient(OSSched *s, OSScClient *c, OSMesgQueue *msgQ, u8 id) {
    if (!s || !c) return;
    c->msgQ = msgQ;
    c->next = s->clientList;
    s->clientList = c;
    /* Capture the VIDEO client's queue: this is the one the N64 VI interrupt
     * feeds retrace messages to at 60 Hz, and the one fb_update drains to
     * measure the elapsed-field count. osRecvMesg drives it from the wall-clock
     * field pacer instead of a (missing) async producer. */
    if (id == OS_SC_ID_VIDEO) {
        s_videoClientQueue = msgQ;
        s_audioRebasePending = false;
    }
}
void osScRemoveClient(OSSched *s, OSScClient *c) { (void)s; (void)c; }
OSMesgQueue *osScGetInterruptQ(OSSched *s) { return s ? &s->interruptQ : NULL; }
OSMesgQueue *osScGetCmdQ(OSSched *s)       { return s ? &s->cmdQ : NULL; }
void func_80079760(OSSched *s)             { (void)s; }

/* ======================================================================== *
 *  Video Interface — the swap/present hook lives in platform_frame_sync
 * ======================================================================== */
static void *s_framebuffer = NULL;
void  osViSwapBuffer(void *fb)              { s_framebuffer = fb; }
void *osViGetNextFramebuffer(void)          { return s_framebuffer; }
void *osViGetCurrentFramebuffer(void)       { return s_framebuffer; }
void  osViSetMode(OSViMode *m)              { (void)m; }
void  osViSetEvent(OSMesgQueue *mq, OSMesg m, u32 rc) { (void)mq; (void)m; (void)rc; }
void  osViBlack(u8 active)                  { (void)active; }
void  osViSetSpecialFeatures(u32 f)         { (void)f; }
void  osViSetYScale(f32 s)                  { (void)s; }
void  osViSetXScale(f32 s)                  { (void)s; }
void  osViRepeatLine(u8 a)                  { (void)a; }
void  osViExtendVStart(u32 v)               { (void)v; }
u32   osViGetCurrentLine(void)              { return 0; }
u32   osViGetStatus(void)                   { return 0; }
void  osCreateViManager(OSPri pri)          { (void)pri; }

/* ======================================================================== *
 *  PI / cartridge DMA — memcpy from the ROM image, instant completion
 * ======================================================================== */
void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 n) {
    (void)pri;
    if (cmdQ) osCreateMesgQueue(cmdQ, cmdBuf, n);
}
OSPiHandle *osCartRomInit(void) { return NULL; }

s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 dir, u32 devAddr,
                 void *dramAddr, u32 size, OSMesgQueue *mq) {
    int readStatus;
    (void)pri; (void)dir;
    /* dramAddr arrives 32-bit-truncated (game does `(u32)ptr`); widen it back
     * to a host pointer inside the arena, then serve the read from the ROM. */
    void *dst = dkr_lo32_to_ptr(mdkr_arena_token_from_host(dramAddr));
    if (!dst) dst = dramAddr;
    readStatus = platform_rom_read(devAddr, dst, (int32_t)size);
    if (mq) osSendMesg(mq, mb ? (OSMesg)mb : (OSMesg)0, OS_MESG_NOBLOCK);
    return readStatus;
}
s32 osPiRawStartDma(s32 dir, u32 devAddr, void *dramAddr, u32 size) {
    int readStatus;
    (void)dir;
    void *dst = dkr_lo32_to_ptr(mdkr_arena_token_from_host(dramAddr));
    if (!dst) dst = dramAddr;
    readStatus = platform_rom_read(devAddr, dst, (int32_t)size);
    return readStatus;
}
u32 osPiGetStatus(void)                 { return 0; }
s32 osPiRawReadIo(u32 a, u32 *d)        { if (d) *d = 0; return 0; }
s32 osPiRawWriteIo(u32 a, u32 d)        { (void)a; (void)d; return 0; }
s32 osPiReadIo(u32 a, u32 *d)           { if (d) *d = 0; return 0; }

/* ======================================================================== *
 *  Timer / counter — host monotonic clock scaled to the N64 COUNTER rate
 * ======================================================================== */
static u64 host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
/* COUNTER ticks at half the CPU clock: 93.75 MHz / 2 = 46.875 MHz. */
#define DKR_COUNTER_HZ 46875000ULL
/* One 60 Hz VI field is exactly 46875000/60 = 781250 COUNTER ticks. */
#define DKR_COUNTER_TICKS_PER_FIELD (DKR_COUNTER_HZ / 60ULL)

/*
 * DETERMINISM: derive the COUNTER from the simulated field clock whenever pacing
 * is synthetic (i.e. --headless-frames), not from the host monotonic clock.
 *
 * On the N64 the COUNTER and the VI retrace rate are both real time, so they are
 * locked to each other. Headless runs break that: the frame loop advances as fast
 * as the machine allows while the pacer synthesises a FIXED field count per
 * frame, so a host-clock COUNTER advances by a machine-load-dependent amount per
 * simulated frame. Game code that integrates COUNTER deltas then produces
 * different results every run -- audio.c music_animation_fraction() does exactly
 * this, and object_functions.c obj_loop_charselect feeds it straight into the
 * character-select models' animFrame. That made headless renders irreproducible:
 * 10 runs of nav_to_character_select gave 10 different frames at the same frame
 * index, median 18.9% of pixels differing, which silently invalidated every
 * frame-comparison check (including oracle scoring) on any screen with
 * music-synced animation.
 *
 * Deriving the COUNTER from the field total keeps the N64's locked relationship
 * and makes headless byte-reproducible. A windowed run still uses the host clock,
 * where it is the correct source.
 *
 * STRICTLY INCREASING (T1, the "crazy fast character-select dance"):
 * the hardware COUNTER advances every second CPU cycle, so two reads in the same
 * frame ALWAYS differ. Both sources above break that assumption -- the synthetic
 * one returns a value that is constant for the whole frame by construction, and
 * the host one is quantised by whatever the platform's clock resolution happens to
 * be (browsers clamp performance.now() to ~100us to defeat timing attacks). Game
 * code that integrates COUNTER deltas cannot survive a zero delta:
 *
 *     if ((u32) audioPrevCount < cnt) { tick += (cnt - audioPrevCount) / 46875.0f; }
 *     else                           { tick += ((cnt - audioPrevCount) - 1) / 46875.0f; }
 *
 * (audio.c music_animation_fraction). Equal values take the else branch, where
 * `0u - 1` is 0xFFFFFFFF, so the tick jumps +91626 ms -- which, modulo the 659 ms
 * beat cycle, lands 22.4 ms BEHIND where it started. Eight character models call
 * that function per frame, so the phase moved -140 ms per frame instead of
 * +16.7 ms: the dance ran backwards through its cycle every 4.7 frames, about 8x
 * too fast, measured against 36-38 frames on the real ROM (tools/anim_period.py).
 *
 * The nudge below costs one tick (21 ns) per extra call in a frame and restores
 * the invariant the game was written against. The comparison is done on the SIGNED
 * difference so a genuine 2^32 wrap (every 91.6 s) still reads as elapsed time and
 * is not mistaken for a repeat -- naively forcing `now > last` would freeze the
 * counter for 91 s at every wrap.
 */
u32 osGetCount(void) {
    static u32 s_lastCount = 0;
    u32 now;

    if (platform_pace_is_synthetic()) {
        now = (u32)(platform_sim_field_count() * DKR_COUNTER_TICKS_PER_FIELD);
    } else {
        now = (u32)((host_ns() * 46875ULL) / 1000000ULL);
    }
    if ((s32)(now - s_lastCount) <= 0) {
        now = s_lastCount + 1;
    }
    s_lastCount = now;
    return now;
}
static u64 s_timeBase = 0;
void osSetTime(u64 t) { s_timeBase = host_ns() - t; }
u64  osGetTime(void)  { return host_ns() - s_timeBase; }
int osSetTimer(OSTimer *t, OSTime c, OSTime i, OSMesgQueue *mq, OSMesg m) {
    (void)t; (void)c; (void)i; (void)mq; (void)m; return 0;
}
int  osStopTimer(OSTimer *t) { (void)t; return 0; }

/* ======================================================================== *
 *  Cache ops / address conversion — no-ops on native
 * ======================================================================== */
void osInvalDCache(void *a, s32 n)     { (void)a; (void)n; }
void osInvalICache(void *a, s32 n)     { (void)a; (void)n; }
void osWritebackDCache(void *a, s32 n) { (void)a; (void)n; }
void osWritebackDCacheAll(void)        { }
u32  osVirtualToPhysical(void *addr)   {
    return mdkr_arena_token_from_host(addr);
}

/* ======================================================================== *
 *  SP / DP status — nothing to drive here (HLE owns rendering)
 * ======================================================================== */
void __osSpSetStatus(u32 s)            { (void)s; }
u32  __osSpGetStatus(void)             { return 0; }
u32  osDpGetStatus(void)               { return 0; }
void osDpSetStatus(u32 s)              { (void)s; }
s32  osDpSetNextBuffer(void *b, u64 n) { (void)b; (void)n; return 0; }

/* ======================================================================== *
 *  Audio Interface — implemented in platform/audi_port_dkr.c (SDL queue +
 *  independent audio-time service). osAiGetStatus/osAiGetLength/osAiSetFrequency/
 *  osAiSetNextBuffer live there now; dkr_audio_service_tick() consumes due
 *  quanta after ordered game ticks below.
 * ======================================================================== */

/* ======================================================================== *
 *  Controllers — live keyboard / SDL controller / scripted input
 * ======================================================================== */
static void controller_query(OSContStatus *data, u8 *bitpattern) {
    u8 pattern = 0;
    if (data) {
        memset(data, 0, sizeof(*data) * MAXCONTROLLERS);
    }
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        const int present = platform_pad_present(i);
        if (present) {
            pattern |= (u8)(1u << i);
        }
        if (data) {
            data[i].type = CONT_TYPE_NORMAL;
            data[i].status = present ? CONT_CARD_ON : 0;
            data[i].errno = present ? 0 : CONT_NO_RESPONSE_ERROR;
        }
    }
    if (bitpattern) {
        *bitpattern = pattern;
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *data) {
    (void)mq;
    controller_query(data, bitpattern);
    if (mdkr_trace_enabled() && bitpattern) {
        MDKR_TRACE("[PADS] present=0x%02x", (unsigned)*bitpattern);
    }
    return 0;
}
s32  osContReset(OSMesgQueue *mq, OSContStatus *d) { (void)mq; controller_query(d, NULL); return 0; }
s32  osContStartQuery(OSMesgQueue *mq)             { (void)mq; return 0; }
s32  osContStartReadData(OSMesgQueue *mq) {
    /* On hardware this kicks an SI DMA that, on completion, posts the registered
     * OS_EVENT_SI message to the controller queue. game/src/joypad.c gates its
     * per-frame osContGetReadData behind a NON-BLOCKING recv on that queue, so
     * without a completion message input is NEVER re-read (the game only ever
     * sees the initial neutral pads — the "menus don't advance" root cause). In
     * the cooperative model the DMA is synchronous, so post the completion now.
     * The queue has capacity 1 and the read/kick cycle keeps it balanced; a
     * best-effort NOBLOCK post is correct if it happens to be full. */
    if (mq) {
        osSendMesg(mq, (OSMesg)(intptr_t)1, OS_MESG_NOBLOCK);
    }
    return 0;
}
void osContGetQuery(OSContStatus *d) { controller_query(d, NULL); }
s32  osContSetCh(u8 ch)                            { (void)ch; return 0; }
void osContGetReadData(OSContPad *pad) {
    static u8 lastPattern = 0xFF;
    u8 pattern = 0;
    if (!pad) return;
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        int sx = 0, sy = 0;
        const int present = platform_pad_present(i);
        if (present) {
            pattern |= (u8)(1u << i);
        }
        pad[i].button = present ? (u16)platform_pad_buttons(i) : 0;
        if (present) {
            platform_pad_stick(i, &sx, &sy);
        }
        pad[i].stick_x = present ? (s8)sx : 0;
        pad[i].stick_y = present ? (s8)sy : 0;
        pad[i].errno   = present ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    if (pattern != lastPattern) {
        if (mdkr_trace_enabled()) {
            MDKR_TRACE("[PADS] present=0x%02x", (unsigned)pattern);
        }
        lastPattern = pattern;
    }
    /* Trace every port that carries input, not just P1: a split-screen route has
     * to be able to prove player 2's pad actually reached the game. */
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        int active = pad[i].button || pad[i].stick_x || pad[i].stick_y;
        if (mdkr_trace_level() >= 3 ? (i == 0 || active) : (mdkr_trace_enabled() && active)) {
            MDKR_TRACE("osContGetReadData P%d btn=0x%04x sx=%d sy=%d @frame~%d",
                       i + 1, pad[i].button, pad[i].stick_x, pad[i].stick_y, g_frameCounter);
        }
    }
}

/* ======================================================================== *
 *  EEPROM — file-backed (per-user save/eeprom.bin). DKR uses 4 Kbit = 512 bytes.
 * ======================================================================== *
 * The save file is UNTRUSTED INPUT. On the web build it comes back out of
 * IndexedDB, where a killed tab, a crashed engine or an interrupted FS.syncfs
 * can leave a short or half-updated image; natively a crash inside the old
 * fopen("wb") (which truncates before anything is written) left a 0-byte or
 * partial file that the next boot happily adopted as canonical. Two changes:
 *
 *  1. STORE ATOMICALLY AND CHECK EVERY STEP. Write eeprom.bin.tmp, flush and
 *     synchronize it, then rename() it over eeprom.bin and synchronize the
 *     directory on native POSIX hosts. The in-memory EEPROM is committed only
 *     after that succeeds. Emscripten's MEMFS rename is atomic; the shell's
 *     single syncfs coalescer then owns persistence to IndexedDB.
 *
 *  2. VALIDATE ON LOAD, AND FAIL TOWARD A CLEAN BOOT. The image is a fixed
 *     table of independently-checksummed blocks (save_layout.h). A block that
 *     is neither "erased" (all 0xFF), nor blank (all 0x00), nor checksum-valid
 *     is not data DKR wrote — it is debris. Debris is quarantined to
 *     eeprom.bin.bad (so nothing is destroyed silently and a bug report can
 *     carry the bytes) and replaced in memory with the state the game itself
 *     defines for "empty": 0xFF for a save slot (what erase_save_file writes),
 *     0x00 for the config and record blocks (which then fail their own
 *     checksum and are ignored). A wrong-length file is rejected whole.
 *
 * Point 2 is deliberately behaviour-NEUTRAL for every image DKR can produce:
 * a valid block is passed through untouched, and a block we blank is one the
 * game already rejected on its own checksum. What changes is that the debris
 * no longer lives on in the file, and the rejection is loud instead of silent.
 * Validation is owned by platform/save_codec.c, the same explicit byte codec
 * used by import/export/editor tooling. See docs/OPEN_ITEMS.md "save failsafe".
 */
#define DKR_EEPROM_BYTES MDKR_SAVE_IMAGE_SIZE
static u8 s_eeprom[DKR_EEPROM_BYTES];
static int s_eepromLoaded = 0;
static int s_eepromPathsReady = 0;
static char s_eepromDir[1024];
static char s_eepromPath[1200];
static char s_eepromTmpPath[1200];
static char s_eepromBadPath[1200];
#define DKR_EEPROM_SNAPSHOT_COUNT 3
static char s_eepromSnapshotPath[DKR_EEPROM_SNAPSHOT_COUNT][1200];
static char s_eepromSnapshotTmpPath[1200];

static int eeprom_init_paths(void) {
    char directory[1024];
    if (s_eepromPathsReady) {
        return 1;
    }
    if (!mdkr_user_save_directory(directory, sizeof(directory))) {
        fprintf(stderr, "[SAVE] save directory is unavailable or too long\n");
        return 0;
    }
    if (snprintf(s_eepromDir, sizeof(s_eepromDir), "%s", directory) < 0 ||
        strlen(directory) >= sizeof(s_eepromDir) ||
        snprintf(s_eepromPath, sizeof(s_eepromPath), "%s/eeprom.bin",
                 directory) < 0 ||
        snprintf(s_eepromTmpPath, sizeof(s_eepromTmpPath), "%s/eeprom.bin.tmp",
                 directory) < 0 ||
        snprintf(s_eepromBadPath, sizeof(s_eepromBadPath), "%s/eeprom.bin.bad",
                 directory) < 0 ||
        snprintf(s_eepromSnapshotTmpPath,
                 sizeof(s_eepromSnapshotTmpPath),
                 "%s/eeprom.bin.autosave.tmp", directory) < 0 ||
        snprintf(s_eepromSnapshotPath[0],
                 sizeof(s_eepromSnapshotPath[0]),
                 "%s/eeprom.bin.autosave.1", directory) < 0 ||
        snprintf(s_eepromSnapshotPath[1],
                 sizeof(s_eepromSnapshotPath[1]),
                 "%s/eeprom.bin.autosave.2", directory) < 0 ||
        snprintf(s_eepromSnapshotPath[2],
                 sizeof(s_eepromSnapshotPath[2]),
                 "%s/eeprom.bin.autosave.3", directory) < 0 ||
        strlen(directory) + sizeof("/eeprom.bin.autosave.tmp") >
            sizeof(s_eepromSnapshotTmpPath)) {
        fprintf(stderr, "[SAVE] save directory path is too long\n");
        return 0;
    }
    s_eepromPathsReady = 1;
    return 1;
}

static int eeprom_ensure_directory(void) {
    struct stat status;
    if (!eeprom_init_paths()) {
        return 0;
    }
    if (stat(s_eepromDir, &status) == 0) {
        if (S_ISDIR(status.st_mode)) {
            return 1;
        }
        fprintf(stderr, "[SAVE] %s exists but is not a directory\n",
                s_eepromDir);
        return 0;
    }
    if (dkr_host_errno() != ENOENT || dkr_fs_mkdir_private(s_eepromDir) != 0) {
        fprintf(stderr, "[SAVE] could not create %s: %s\n", s_eepromDir,
                strerror(dkr_host_errno()));
        return 0;
    }
    return 1;
}

static int eeprom_image_is_safe(const u8 image[DKR_EEPROM_BYTES]) {
    MdkrSaveDocument document;
    int i;

    if (image == NULL ||
        mdkr_save_decode(image, DKR_EEPROM_BYTES, &document) != MDKR_SAVE_OK) {
        return 0;
    }
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        if (document.block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
            return 0;
        }
    }
    return 1;
}

static int eeprom_read_exact(const char *path,
                             u8 image[DKR_EEPROM_BYTES]) {
    FILE *file;
    size_t got;
    int trailing;

    if (path == NULL || image == NULL) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    got = fread(image, 1, DKR_EEPROM_BYTES, file);
    trailing = fgetc(file);
    if (ferror(file) || fclose(file) != 0 ||
        got != DKR_EEPROM_BYTES || trailing != EOF) {
        return 0;
    }
    return 1;
}

static int eeprom_write_exact(const char *temporary_path,
                              const char *final_path,
                              const u8 image[DKR_EEPROM_BYTES]) {
    FILE *file;
    size_t wrote;
    int failed = 0;

    file = fopen(temporary_path, "wb");
    if (file == NULL) {
        return 0;
    }
    wrote = fwrite(image, 1, DKR_EEPROM_BYTES, file);
    if (wrote != DKR_EEPROM_BYTES || fflush(file) != 0) {
        failed = 1;
    }
    if (!failed && dkr_fs_sync_file(file) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (!failed && dkr_fs_replace(temporary_path, final_path) != 0) {
        failed = 1;
    }
    if (failed) {
        (void) remove(temporary_path);
        return 0;
    }
    return 1;
}

/*
 * Retain the three most recent distinct, checksum-safe EEPROM generations.
 *
 * Do not rotate filenames destructively. A failed rename-based chain can lose
 * an older point before the newer one has been installed, and it also carries
 * forward any damaged file already occupying the ring. Instead, first build a
 * packed in-memory set from the safe live image plus safe existing points, then
 * atomically replace each destination from the oldest to the newest. A crash
 * between replacements can leave the ring slightly out of order, but every
 * visible point is independently complete and checksum-safe, and the live image
 * is never touched by snapshot maintenance.
 */
static void eeprom_rotate_snapshots(void) {
    u8 generations[DKR_EEPROM_SNAPSHOT_COUNT][DKR_EEPROM_BYTES];
    u8 candidate[DKR_EEPROM_BYTES];
    int generation_count = 0;
    int source;
    int destination;

    if (!eeprom_read_exact(s_eepromPath, candidate) ||
        !eeprom_image_is_safe(candidate)) {
        return;
    }
    memcpy(generations[generation_count++], candidate, sizeof(candidate));

    for (source = 0;
         source < DKR_EEPROM_SNAPSHOT_COUNT &&
             generation_count < DKR_EEPROM_SNAPSHOT_COUNT;
         source++) {
        int duplicate = 0;
        int prior;

        if (!eeprom_read_exact(s_eepromSnapshotPath[source], candidate) ||
            !eeprom_image_is_safe(candidate)) {
            continue;
        }
        for (prior = 0; prior < generation_count; prior++) {
            if (memcmp(generations[prior], candidate, sizeof(candidate)) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            memcpy(generations[generation_count++], candidate,
                   sizeof(candidate));
        }
    }

    for (destination = generation_count - 1; destination >= 0; destination--) {
        if (!eeprom_write_exact(s_eepromSnapshotTmpPath,
                                s_eepromSnapshotPath[destination],
                                generations[destination])) {
            fprintf(stderr,
                    "[SAVE] could not retain automatic save snapshot %d\n",
                    destination + 1);
        }
    }
    for (destination = generation_count;
         destination < DKR_EEPROM_SNAPSHOT_COUNT;
         destination++) {
        /*
         * There is no corresponding safe generation. Remove a stale/corrupt
         * tail rather than advertising it as a recovery point.
         */
        (void) remove(s_eepromSnapshotPath[destination]);
    }
}

static int eeprom_store_image_internal(
    const u8 image[DKR_EEPROM_BYTES],
    int retain_previous) {
    FILE *file;
    size_t wrote;
    int failed = 0;
#ifndef __EMSCRIPTEN__
    int directory_sync_failed = 0;
#endif

    if (image == NULL || !eeprom_ensure_directory()) {
        return 0;
    }
    file = fopen(s_eepromTmpPath, "wb");
    if (file == NULL) {
        fprintf(stderr, "[SAVE] could not open %s: %s\n", s_eepromTmpPath,
                strerror(dkr_host_errno()));
        return 0;
    }
    wrote = fwrite(image, 1, DKR_EEPROM_BYTES, file);
    if (wrote != DKR_EEPROM_BYTES || fflush(file) != 0) {
        failed = 1;
    }
    if (!failed && dkr_fs_sync_file(file) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (!failed && retain_previous) {
        eeprom_rotate_snapshots();
    }
    if (!failed && dkr_fs_replace(s_eepromTmpPath, s_eepromPath) != 0) {
        failed = 1;
    }
#ifndef __EMSCRIPTEN__
    if (!failed && dkr_fs_sync_dir(s_eepromDir) != 0) {
        directory_sync_failed = 1;
    }
#endif
    if (failed) {
        int saved_errno = dkr_host_errno();
        (void) remove(s_eepromTmpPath);
        fprintf(stderr, "[SAVE] durable write of %s failed: %s\n",
                s_eepromPath, strerror(saved_errno));
        return 0;
    }
#ifndef __EMSCRIPTEN__
    if (directory_sync_failed) {
        /*
         * rename() has already installed the new complete image. Reporting the
         * whole transaction as failed now would make the caller retain the old
         * in-memory generation while subsequent reads see the new file. Keep
         * live state coherent and report only the reduced crash-durability
         * guarantee.
         */
        fprintf(stderr,
                "[SAVE] warning: %s is installed, but the save directory "
                "could not be synchronized\n",
                s_eepromPath);
    }
#endif
#ifdef __EMSCRIPTEN__
    if (!mdkr_persist_save_async(0)) {
        /*
         * Keep the new in-memory generation live and let the shell's retry
         * timer persist it later, but make the durability failure explicit.
         * Returning failure here would cause the caller to retain old EEPROM
         * state while the filesystem already contains the new image.
         */
        fprintf(stderr,
                "[SAVE] browser storage did not acknowledge this generation; "
                "automatic retry remains armed\n");
    }
#endif
    return 1;
}

static int eeprom_store_image(const u8 image[DKR_EEPROM_BYTES]) {
    return eeprom_store_image_internal(image, 1);
}

/* Copy the bytes we are about to reject to save/eeprom.bin.bad. Best effort:
 * failing to quarantine must not stop the clean boot. */
static void eeprom_quarantine(const u8 *bytes, size_t n) {
    FILE *f;
    if (bytes == NULL || !eeprom_ensure_directory()) {
        return;
    }
    f = fopen(s_eepromBadPath, "wb");
    if (f == NULL) {
        return;
    }
    if (n != 0) {
        if (fwrite(bytes, 1, n, f) != n) {
            fclose(f);
            return;
        }
    }
    if (fclose(f) == 0) {
        fprintf(stderr, "[SAVE] quarantined %zu bad byte(s) to %s\n", n,
                s_eepromBadPath);
    }
}

/* Returns the number of blocks blanked. */
static int eeprom_sanitize(void) {
    MdkrSaveDocument document;
    MdkrSaveDocument recovered;
    MdkrRecoveryPlan plan;
    int reset_count = 0;
    int i;

    if (mdkr_save_decode(s_eeprom, sizeof(s_eeprom), &document) !=
        MDKR_SAVE_OK) {
        return 0; /* Fixed-size in-memory image: unreachable defensive guard. */
    }
    memset(&plan, 0, sizeof(plan));
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        if (document.block_status[i] != MDKR_SAVE_BLOCK_CORRUPT) {
            continue;
        }
        plan.block_action[i] = MDKR_SAVE_RECOVER_RESET;
        reset_count++;
        if (i < (int) MDKR_SAVE_SLOT_COUNT) {
            fprintf(stderr,
                    "[SAVE] save slot %d is corrupt or semantically unsafe - "
                    "erasing it\n",
                    i);
        } else if (i == MDKR_SAVE_BLOCK_CONFIG) {
            fprintf(stderr,
                    "[SAVE] options block is corrupt - resetting it\n");
        } else {
            fprintf(stderr, "[SAVE] %s records block is corrupt - dropping it\n",
                    i == MDKR_SAVE_BLOCK_FAST_LAPS ? "fastest-lap"
                                                   : "course-time");
        }
    }
    if (reset_count != 0) {
        if (mdkr_save_recover(s_eeprom, sizeof(s_eeprom), &plan, &recovered) !=
            MDKR_SAVE_OK) {
            fprintf(stderr,
                    "[SAVE] recovery failed unexpectedly - using a blank "
                    "image instead\n");
            memset(s_eeprom, 0, sizeof(s_eeprom));
        } else {
            memcpy(s_eeprom, recovered.bytes, sizeof(s_eeprom));
        }
    }
    return reset_count;
}

static void eeprom_block_span(int block, size_t *offset, size_t *size) {
    static const size_t offsets[MDKR_SAVE_BLOCK_COUNT] = {
        0u,
        MDKR_SAVE_SLOT_SIZE,
        MDKR_SAVE_SLOT_SIZE * 2u,
        MDKR_SAVE_SLOT_SIZE * 3u,
        128u,
        128u + MDKR_SAVE_RECORD_BLOCK_SIZE,
    };
    static const size_t sizes[MDKR_SAVE_BLOCK_COUNT] = {
        MDKR_SAVE_SLOT_SIZE,
        MDKR_SAVE_SLOT_SIZE,
        MDKR_SAVE_SLOT_SIZE,
        8u,
        MDKR_SAVE_RECORD_BLOCK_SIZE,
        MDKR_SAVE_RECORD_BLOCK_SIZE,
    };

    *offset = offsets[block];
    *size = sizes[block];
}

/* Restore only damaged checksum domains, retaining every valid byte from the
 * newest live image. This avoids trading a recent valid race result in one slot
 * for an older whole-image snapshot merely because an unrelated record block
 * was damaged. */
static int eeprom_repair_from_snapshots(void) {
    MdkrSaveDocument current;
    MdkrSaveDocument snapshot;
    u8 snapshot_bytes[DKR_EEPROM_BYTES];
    int repaired = 0;
    int block;
    int generation;

    if (mdkr_save_decode(s_eeprom, sizeof(s_eeprom), &current) !=
        MDKR_SAVE_OK) {
        return 0;
    }
    for (block = 0; block < MDKR_SAVE_BLOCK_COUNT; block++) {
        size_t offset;
        size_t size;

        if (current.block_status[block] != MDKR_SAVE_BLOCK_CORRUPT) {
            continue;
        }
        eeprom_block_span(block, &offset, &size);
        for (generation = 0;
             generation < DKR_EEPROM_SNAPSHOT_COUNT;
             generation++) {
            if (!eeprom_read_exact(s_eepromSnapshotPath[generation],
                                   snapshot_bytes) ||
                mdkr_save_decode(snapshot_bytes, sizeof(snapshot_bytes),
                                 &snapshot) != MDKR_SAVE_OK ||
                snapshot.block_status[block] == MDKR_SAVE_BLOCK_CORRUPT) {
                continue;
            }
            memcpy(s_eeprom + offset, snapshot_bytes + offset, size);
            repaired++;
            fprintf(stderr,
                    "[SAVE] restored block %d from automatic snapshot %d\n",
                    block, generation + 1);
            break;
        }
    }
    return repaired;
}

static int eeprom_restore_latest_snapshot(void) {
    u8 snapshot[DKR_EEPROM_BYTES];
    int generation;

    for (generation = 0;
         generation < DKR_EEPROM_SNAPSHOT_COUNT;
         generation++) {
        if (!eeprom_read_exact(s_eepromSnapshotPath[generation], snapshot) ||
            !eeprom_image_is_safe(snapshot)) {
            continue;
        }
        memcpy(s_eeprom, snapshot, sizeof(s_eeprom));
        fprintf(stderr,
                "[SAVE] restored complete EEPROM from automatic snapshot %d\n",
                generation + 1);
        (void) eeprom_store_image_internal(s_eeprom, 0);
        return 1;
    }
    return 0;
}

static void eeprom_load(void) {
    FILE *f;
    u8 buf[DKR_EEPROM_BYTES + 1];
    size_t got;
    int repaired;
    int reset;

    if (s_eepromLoaded) return;
    s_eepromLoaded = 1;
    if (!eeprom_init_paths()) {
        return;
    }
    f = fopen(s_eepromPath, "rb");
    if (f == NULL) {
        return;   /* no save yet — a blank EEPROM is exactly a fresh cart */
    }
    /* Read one byte more than the image so a too-LONG file is caught too. */
    got = fread(buf, 1, sizeof(buf), f);
    if (ferror(f)) {
        fprintf(stderr, "[SAVE] failed while reading %s: %s\n", s_eepromPath,
                strerror(dkr_host_errno()));
        got = 0;
    }
    (void) fclose(f);
    if (got != DKR_EEPROM_BYTES) {
        /* Torn or foreign file. Do not interpret one byte of it. (For a too-long
         * file the quarantine keeps the first 513 bytes rather than the lot —
         * nothing in the port can produce one, and that is enough to diagnose.) */
        fprintf(stderr, "[SAVE] %s is %zu bytes, expected %d - ignoring it and "
                        "starting from a blank save\n",
                s_eepromPath, got, DKR_EEPROM_BYTES);
        eeprom_quarantine(buf, got);
        if (eeprom_restore_latest_snapshot()) {
            return;
        }
        memset(s_eeprom, 0, DKR_EEPROM_BYTES);
        (void) eeprom_store_image_internal(s_eeprom, 0);
        return;
    }
    memcpy(s_eeprom, buf, DKR_EEPROM_BYTES);
    repaired = eeprom_repair_from_snapshots();
    reset = eeprom_sanitize();
    if (repaired != 0 || reset != 0) {
        eeprom_quarantine(buf, DKR_EEPROM_BYTES);
        (void) eeprom_store_image_internal(s_eeprom, 0);
    }
}

static int eeprom_bounds(u8 addr, s32 byte_count, const void *buffer,
                         size_t *offset_out) {
    size_t offset = (size_t) addr * EEPROM_BLOCK_SIZE;
    if (buffer == NULL || offset > DKR_EEPROM_BYTES || byte_count < 0 ||
        (size_t) byte_count > DKR_EEPROM_BYTES - offset) {
        return 0;
    }
    *offset_out = offset;
    return 1;
}

s32 osEepromProbe(OSMesgQueue *mq) { (void)mq; return EEPROM_TYPE_4K; }
s32 osEepromRead(OSMesgQueue *mq, u8 addr, u8 *buf) {
    size_t offset;
    (void) mq;
    eeprom_load();
    if (!eeprom_bounds(addr, EEPROM_BLOCK_SIZE, buf, &offset)) {
        return -1;
    }
    memcpy(buf, s_eeprom + offset, EEPROM_BLOCK_SIZE);
    return 0;
}
s32 osEepromWrite(OSMesgQueue *mq, u8 addr, u8 *buf) {
    size_t offset;
    u8 candidate[DKR_EEPROM_BYTES];
    (void) mq;
    eeprom_load();
    if (!eeprom_bounds(addr, EEPROM_BLOCK_SIZE, buf, &offset)) {
        return -1;
    }
    memcpy(candidate, s_eeprom, sizeof(candidate));
    memcpy(candidate + offset, buf, EEPROM_BLOCK_SIZE);
    if (!eeprom_store_image(candidate)) {
        return -1;
    }
    memcpy(s_eeprom, candidate, sizeof(s_eeprom));
    return 0;
}
s32 osEepromLongRead(OSMesgQueue *mq, u8 addr, u8 *buf, s32 n) {
    size_t offset;
    (void) mq;
    eeprom_load();
    if (!eeprom_bounds(addr, n, buf, &offset)) {
        return -1;
    }
    memcpy(buf, s_eeprom + offset, (size_t) n);
    return 0;
}
s32 osEepromLongWrite(OSMesgQueue *mq, u8 addr, u8 *buf, s32 n) {
    size_t offset;
    u8 candidate[DKR_EEPROM_BYTES];
    (void) mq;
    eeprom_load();
    if (!eeprom_bounds(addr, n, buf, &offset)) {
        return -1;
    }
    memcpy(candidate, s_eeprom, sizeof(candidate));
    memcpy(candidate + offset, buf, (size_t) n);
    if (!eeprom_store_image(candidate)) {
        return -1;
    }
    memcpy(s_eeprom, candidate, sizeof(s_eeprom));
    return 0;
}

/* ======================================================================== *
 *  Controller Pak (PFS) — bounded, checksummed virtual 32 KiB packs
 * ======================================================================== *
 * One pack image is persisted per controller in the same durable save mount as
 * EEPROM. The wire format lives in virtual_pak.c: no host struct layouts,
 * pointers, endian assumptions, or unchecked extents cross the file boundary.
 * Mutations are copy-on-write and become live only after an atomic file replace.
 */
static MdkrVirtualPak s_virtualPaks[MAXCONTROLLERS];
static int s_virtualPakState[MAXCONTROLLERS]; /* 0 unopened, 1 good, -1 corrupt */
static char s_virtualPakPath[MAXCONTROLLERS][1200];
static char s_virtualPakTmpPath[MAXCONTROLLERS][1200];

static int virtual_pak_error(MdkrVirtualPakResult result) {
    switch (result) {
        case MDKR_VPAK_OK: return 0;
        case MDKR_VPAK_ERR_NOT_FOUND:
        case MDKR_VPAK_ERR_ARGUMENT:
        case MDKR_VPAK_ERR_RANGE: return PFS_ERR_INVALID;
        case MDKR_VPAK_ERR_EXISTS: return PFS_ERR_EXIST;
        case MDKR_VPAK_ERR_DATA_FULL: return PFS_DATA_FULL;
        case MDKR_VPAK_ERR_DIR_FULL: return PFS_DIR_FULL;
        case MDKR_VPAK_ERR_DIGEST:
        case MDKR_VPAK_ERR_FORMAT:
        default: return PFS_ERR_BAD_DATA;
    }
}

static int virtual_pak_paths(int channel) {
    int path_length;
    int temporary_path_length;
    if (channel < 0 || channel >= MAXCONTROLLERS ||
        !eeprom_ensure_directory()) {
        return 0;
    }
    path_length = snprintf(
        s_virtualPakPath[channel], sizeof(s_virtualPakPath[channel]),
        "%s/controller-pak-%d.mdp", s_eepromDir, channel + 1);
    temporary_path_length = snprintf(
        s_virtualPakTmpPath[channel], sizeof(s_virtualPakTmpPath[channel]),
        "%s/controller-pak-%d.mdp.tmp", s_eepromDir, channel + 1);
    if (path_length < 0 ||
        (size_t)path_length >= sizeof(s_virtualPakPath[channel]) ||
        temporary_path_length < 0 ||
        (size_t)temporary_path_length >=
            sizeof(s_virtualPakTmpPath[channel])) {
        fprintf(stderr, "[PFS] save directory path is too long\n");
        return 0;
    }
    return 1;
}

static int virtual_pak_store(int channel, const MdkrVirtualPak *pak) {
    uint8_t image[MDKR_VPAK_IMAGE_SIZE];
    FILE *file;
    size_t wrote;
    int failed = 0;
#ifndef __EMSCRIPTEN__
    int directory_sync_failed = 0;
#endif
    if (pak == NULL || !virtual_pak_paths(channel) ||
        mdkr_virtual_pak_encode(pak, image, sizeof(image)) != MDKR_VPAK_OK) {
        return 0;
    }
    file = fopen(s_virtualPakTmpPath[channel], "wb");
    if (file == NULL) {
        fprintf(stderr, "[PFS] could not open %s: %s\n",
                s_virtualPakTmpPath[channel], strerror(dkr_host_errno()));
        return 0;
    }
    wrote = fwrite(image, 1, sizeof(image), file);
    if (wrote != sizeof(image) || fflush(file) != 0) {
        failed = 1;
    }
    if (!failed && dkr_fs_sync_file(file) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (!failed &&
        dkr_fs_replace(s_virtualPakTmpPath[channel],
                       s_virtualPakPath[channel]) != 0) {
        failed = 1;
    }
#ifndef __EMSCRIPTEN__
    if (!failed && dkr_fs_sync_dir(s_eepromDir) != 0) {
        directory_sync_failed = 1;
    }
#endif
    if (failed) {
        int saved_errno = dkr_host_errno();
        (void)remove(s_virtualPakTmpPath[channel]);
        fprintf(stderr, "[PFS] durable write of %s failed: %s\n",
                s_virtualPakPath[channel], strerror(saved_errno));
        return 0;
    }
#ifndef __EMSCRIPTEN__
    if (directory_sync_failed) {
        fprintf(stderr,
                "[PFS] warning: %s is installed, but the save directory "
                "could not be synchronized\n",
                s_virtualPakPath[channel]);
    }
#endif
#ifdef __EMSCRIPTEN__
    if (!mdkr_persist_save_async(1)) {
        fprintf(stderr,
                "[PFS] browser storage did not acknowledge this generation; "
                "automatic retry remains armed\n");
    }
#endif
    return 1;
}

static void virtual_pak_quarantine(int channel) {
    char bad_path[1200];
    struct stat status;
    int suffix;
    for (suffix = 1; suffix <= 99; suffix++) {
        if (snprintf(
                bad_path, sizeof(bad_path), "%s.bad.%d",
                s_virtualPakPath[channel], suffix) < 0) {
            return;
        }
        if (stat(bad_path, &status) != 0 && dkr_host_errno() == ENOENT) {
            if (rename(s_virtualPakPath[channel], bad_path) == 0) {
                fprintf(stderr, "[PFS] quarantined corrupt pack as %s\n",
                        bad_path);
#ifdef __EMSCRIPTEN__
                (void)mdkr_persist_save_async(1);
#endif
            }
            return;
        }
    }
    fprintf(stderr,
            "[PFS] corrupt pack retained in place; quarantine slots are full\n");
}

static int virtual_pak_load(int channel) {
    uint8_t image[MDKR_VPAK_IMAGE_SIZE];
    FILE *file;
    size_t got;
    int trailing;
    MdkrVirtualPakResult decoded;
    if (channel < 0 || channel >= MAXCONTROLLERS ||
        !platform_pad_present(channel)) {
        return PFS_ERR_NOPACK;
    }
    if (s_virtualPakState[channel] > 0) return 0;
    if (s_virtualPakState[channel] < 0) return PFS_ERR_BAD_DATA;
    if (!virtual_pak_paths(channel)) return PFS_ERR_DEVICE;
    file = fopen(s_virtualPakPath[channel], "rb");
    if (file == NULL) {
        if (dkr_host_errno() == ENOENT) {
            mdkr_virtual_pak_init(&s_virtualPaks[channel]);
            s_virtualPakState[channel] = 1;
            return 0;
        }
        return PFS_ERR_DEVICE;
    }
    got = fread(image, 1, sizeof(image), file);
    trailing = fgetc(file);
    if (ferror(file) || fclose(file) != 0 ||
        got != sizeof(image) || trailing != EOF) {
        decoded = MDKR_VPAK_ERR_FORMAT;
    } else {
        decoded = mdkr_virtual_pak_decode(
            image, sizeof(image), &s_virtualPaks[channel]);
    }
    if (decoded != MDKR_VPAK_OK) {
        fprintf(stderr, "[PFS] rejected corrupt pack %s (error %d)\n",
                s_virtualPakPath[channel], (int)decoded);
        virtual_pak_quarantine(channel);
        mdkr_virtual_pak_init(&s_virtualPaks[channel]);
        s_virtualPakState[channel] = -1;
        return PFS_ERR_BAD_DATA;
    }
    s_virtualPakState[channel] = 1;
    return 0;
}

/* Validates pfs and resolves the channel's pak. `pak` is an out-parameter
 * because the return value carries the PFS status, and it must be written on
 * EVERY path, including the failure paths: the callers all read it only after
 * checking the status, and the unconditional store is what keeps definite
 * assignment provable at each call site. Making the store conditional on
 * `pak != NULL` cost that proof -- GCC 11 rejects the resulting
 * -Wmaybe-uninitialized read in osPfsNumFiles under -Werror. Callers that want
 * the status alone use virtual_pak_check(). */
static int virtual_pak_handle(OSPfs *pfs, MdkrVirtualPak **pak) {
    int result;

    *pak = NULL;
    if (pfs == NULL || pfs->channel < 0 ||
        pfs->channel >= MAXCONTROLLERS) {
        return PFS_ERR_INVALID;
    }
    result = virtual_pak_load(pfs->channel);
    if (result != 0) return result;
    if (!(pfs->status & PFS_INITIALIZED)) return PFS_ERR_INVALID;
    *pak = &s_virtualPaks[pfs->channel];
    return 0;
}

/* The same validation for callers that need the status and not the pak. */
static int virtual_pak_check(OSPfs *pfs) {
    MdkrVirtualPak *pak;

    return virtual_pak_handle(pfs, &pak);
}

s32 osPfsInitPak(OSMesgQueue *mq, OSPfs *pfs, s32 ch) {
    return osPfsInit(mq, pfs, ch);
}
s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, s32 ch) {
    int result;
    if (pfs == NULL || ch < 0 || ch >= MAXCONTROLLERS) {
        return PFS_ERR_INVALID;
    }
    result = virtual_pak_load(ch);
    if (result != 0) return result;
    pfs->queue = mq;
    pfs->channel = ch;
    pfs->status |= PFS_INITIALIZED;
    pfs->version = OS_PFS_VERSION;
    pfs->dir_size = MDKR_VPAK_MAX_FILES;
    pfs->banks = PFS_BANKS_256K;
    return 0;
}
s32 osPfsRepairId(OSPfs *pfs) {
    return virtual_pak_check(pfs);
}
s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern) {
    u8 found = 0;
    (void)mq;
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        if (platform_pad_present(i) || platform_pad_rumble_supported(i)) {
            found |= (u8)(1u << i);
        }
    }
    if (pattern) *pattern = found;
    return 0;
}
s32 osPfsAllocateFile(OSPfs *p, u16 cc, u32 gc, u8 *gn, u8 *en, s32 sz, s32 *no) {
    MdkrVirtualPak *pak;
    MdkrVirtualPak candidate;
    MdkrVirtualPakResult result;
    int status = virtual_pak_handle(p, &pak);
    if (no) *no = -1;
    if (status != 0) return status;
    if (gn == NULL || en == NULL || sz <= 0) return PFS_ERR_INVALID;
    candidate = *pak;
    result = mdkr_virtual_pak_allocate(
        &candidate, cc, gc, gn, en, (uint32_t)sz, no);
    if (result != MDKR_VPAK_OK) return virtual_pak_error(result);
    if (!virtual_pak_store(p->channel, &candidate)) return PFS_ERR_DEVICE;
    *pak = candidate;
    return 0;
}
s32 osPfsFindFile(OSPfs *p, u16 cc, u32 gc, u8 *gn, u8 *en, s32 *no) {
    MdkrVirtualPak *pak;
    MdkrVirtualPakResult result;
    int status = virtual_pak_handle(p, &pak);
    if (no) *no = -1;
    if (status != 0) return status;
    if (gn == NULL || en == NULL) return PFS_ERR_INVALID;
    result = mdkr_virtual_pak_find(pak, cc, gc, gn, en, no);
    return virtual_pak_error(result);
}
s32 osPfsDeleteFile(OSPfs *p, u16 cc, u32 gc, u8 *gn, u8 *en) {
    MdkrVirtualPak *pak;
    MdkrVirtualPak candidate;
    MdkrVirtualPakResult result;
    int status = virtual_pak_handle(p, &pak);
    if (status != 0) return status;
    if (gn == NULL || en == NULL) return PFS_ERR_INVALID;
    candidate = *pak;
    result = mdkr_virtual_pak_delete(&candidate, cc, gc, gn, en);
    if (result != MDKR_VPAK_OK) return virtual_pak_error(result);
    if (!virtual_pak_store(p->channel, &candidate)) return PFS_ERR_DEVICE;
    *pak = candidate;
    return 0;
}
s32 osPfsReadWriteFile(OSPfs *p, s32 no, u8 fl, s32 off, s32 n, u8 *buf) {
    MdkrVirtualPak *pak;
    MdkrVirtualPak candidate;
    MdkrVirtualPakResult result;
    int status = virtual_pak_handle(p, &pak);
    if (status != 0) return status;
    if (off < 0 || n < 0 || (buf == NULL && n != 0)) return PFS_ERR_INVALID;
    if (fl == PFS_READ) {
        result = mdkr_virtual_pak_read(
            pak, no, (uint32_t)off, buf, (uint32_t)n);
        return virtual_pak_error(result);
    }
    if (fl != PFS_WRITE) return PFS_ERR_INVALID;
    candidate = *pak;
    result = mdkr_virtual_pak_write(
        &candidate, no, (uint32_t)off, buf, (uint32_t)n);
    if (result != MDKR_VPAK_OK) return virtual_pak_error(result);
    if (!virtual_pak_store(p->channel, &candidate)) return PFS_ERR_DEVICE;
    *pak = candidate;
    return 0;
}
s32 osPfsFileState(OSPfs *p, s32 no, OSPfsState *st) {
    MdkrVirtualPak *pak;
    MdkrVirtualPakFile state;
    MdkrVirtualPakResult result;
    int status = virtual_pak_handle(p, &pak);
    if (status != 0) return status;
    if (st == NULL) return PFS_ERR_INVALID;
    result = mdkr_virtual_pak_file_state(pak, no, &state);
    if (result != MDKR_VPAK_OK) return virtual_pak_error(result);
    memset(st, 0, sizeof(*st));
    st->file_size = state.size;
    st->game_code = state.game_code;
    st->company_code = state.company_code;
    memcpy(st->game_name, state.game_name, sizeof(st->game_name));
    memcpy(st->ext_name, state.ext_name, sizeof(st->ext_name));
    return 0;
}
s32 osPfsFreeBlocks(OSPfs *p, s32 *bytes) {
    MdkrVirtualPak *pak;
    int status = virtual_pak_handle(p, &pak);
    if (bytes) *bytes = 0;
    if (status != 0) return status;
    if (bytes) *bytes = (s32)mdkr_virtual_pak_free_bytes(pak);
    return 0;
}
s32 osPfsNumFiles(OSPfs *p, s32 *maxf, s32 *used) {
    MdkrVirtualPak *pak;
    int status = virtual_pak_handle(p, &pak);
    if (maxf) *maxf = 0;
    if (used) *used = 0;
    if (status != 0) return status;
    if (maxf) *maxf = MDKR_VPAK_MAX_FILES;
    if (used) *used = mdkr_virtual_pak_used_files(pak);
    return 0;
}
s32 osPfsChecker(OSPfs *pfs) {
    return virtual_pak_check(pfs);
}
s32 osPfsReFormat(OSPfs *pfs, OSMesgQueue *mq, s32 ch) {
    MdkrVirtualPak blank;
    uint64_t generation = 0;
    if (pfs == NULL || ch < 0 || ch >= MAXCONTROLLERS ||
        !platform_pad_present(ch)) {
        return PFS_ERR_NOPACK;
    }
    if (s_virtualPakState[ch] > 0) {
        generation = s_virtualPaks[ch].generation;
    }
    mdkr_virtual_pak_init(&blank);
    blank.generation = generation + 1;
    if (!virtual_pak_store(ch, &blank)) return PFS_ERR_DEVICE;
    s_virtualPaks[ch] = blank;
    s_virtualPakState[ch] = 1;
    pfs->queue = mq;
    pfs->channel = ch;
    pfs->status |= PFS_INITIALIZED;
    pfs->version = OS_PFS_VERSION;
    pfs->dir_size = MDKR_VPAK_MAX_FILES;
    pfs->banks = PFS_BANKS_256K;
    return 0;
}

/* ======================================================================== *
 *  Rumble Pak — SDL haptics / browser Gamepad vibration
 * ======================================================================== */
s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, s32 ch) {
    if (!pfs || ch < 0 || ch >= MAXCONTROLLERS ||
        !platform_pad_rumble_supported(ch)) {
        return PFS_ERR_NOPACK;
    }
    pfs->queue = mq;
    pfs->channel = ch;
    pfs->status |= PFS_MOTOR_INITIALIZED;
    return 0;
}
s32 osMotorStart(OSPfs *pfs) {
    if (!pfs || !(pfs->status & PFS_MOTOR_INITIALIZED) ||
        !platform_pad_rumble(pfs->channel, 1)) {
        return PFS_ERR_NOPACK;
    }
    return 0;
}
s32 osMotorStop(OSPfs *pfs) {
    if (!pfs || !(pfs->status & PFS_MOTOR_INITIALIZED) ||
        !platform_pad_rumble(pfs->channel, 0)) {
        return PFS_ERR_NOPACK;
    }
    return 0;
}

/* ======================================================================== *
 *  Misc / debug
 * ======================================================================== */
void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg m) { (void)e; (void)mq; (void)m; }
void osSyncPrintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
void osLogEvent(OSLog *log, s16 code, s16 numArgs, ...) { (void)log; (void)code; (void)numArgs; }
u32  __osGetFpcCsr(void)      { return 0; }
void __osSetFpcCsr(u32 v)     { (void)v; }

/* ======================================================================== *
 *  MDKR_LOAD_TRACK test hook -- race a different track than the menu chose
 * ======================================================================== *
 * Parsed here rather than in game/src/game.c deliberately: that translation
 * unit includes the N64 SDK headers, whose <PR/os_libc.h> declares its own
 * memmove/memcpy, and pulling <stdlib.h>/<string.h> in ahead of them breaks the
 * build. game.c reaches this through a plain extern, the same way it already
 * reaches mdkr_trace()/mdkr_trace_enabled().
 *
 * Returns the forced level id, or -1 when MDKR_LOAD_TRACK is unset (the hook is
 * then a no-op). *vehicleOut receives the forced vehicle id, or -1 for "leave
 * the menu's choice alone". Format: MDKR_LOAD_TRACK=<levelId>[:<vehicle>].
 */
int mdkr_force_track(int *vehicleOut) {
    static int sTrack = -2;
    static int sVehicle = -1;
    if (sTrack == -2) {
        const char *e = getenv("MDKR_LOAD_TRACK");
        sTrack = -1;
        if (e != NULL && e[0] != '\0') {
            const char *colon = strchr(e, ':');
            sTrack = atoi(e);
            if (colon != NULL && colon[1] != '\0') {
                sVehicle = atoi(colon + 1);
            }
        }
    }
    if (vehicleOut != NULL) {
        *vehicleOut = sVehicle;
    }
    return sTrack;
}

/*
 * GAME-08 positive-control seam. The retail menu loader hard-coded plane for
 * every rolling demo, even when the level header selects car or hovercraft.
 * Keep the historical arm reachable from one binary so the attract-mode gate
 * can prove that the selected AI path is load-bearing.
 */
int mdkr_menu_vehicle_legacy(void) {
    static int sLegacy = -1;
    if (sLegacy < 0) {
        const char *e = getenv("MDKR_MENU_VEHICLE");
        sLegacy = e != NULL && strcasecmp(e, "legacy") == 0;
    }
    return sLegacy;
}

/* ======================================================================== *
 *  MDKR_GRIDMASK=off -- A/B the collision grid-mask Z-row fix
 * ======================================================================== *
 * compute_grid_overlap_mask() (game/src/hasm/collision.c) builds a coarse 8x8
 * occupancy mask that pre-filters which terrain triangles even get considered
 * for collision. The upstream NON_MATCHING C body's Z half compared the query
 * rectangle against a value the clamping above had already forced -- a
 * tautology -- so every Z row from the first accepted one through row 7 was
 * set and the Z filter did nothing. The ROM compares the rolling per-row
 * cell_z (see the comment at that line).
 *
 * The consequence is SILENT: an over-permissive pre-filter still yields correct
 * collisions, just far too many candidates, and
 * generate_collision_candidates() TRUNCATES the list at
 * MAX_COLLISION_CANDIDATES (500) with a `goto out`. Only where the search
 * rectangle sees more than 500 collidable triangles does anything go wrong --
 * and then the ground the racer is standing on can be in the discarded tail, so
 * the racer falls through the level.
 *
 * Setting MDKR_GRIDMASK=off restores the tautology exactly, which is what makes
 * tests/check_collision_gridmask.py a real check rather than a vacuous one: it
 * renders BOTH arms from the same binary and requires the broken arm to
 * actually reproduce the fall. Same contract as MDKR_NEARCLIP=off /
 * MDKR_LINESWAP=off. No-op unless set.
 */
int mdkr_gridmask_legacy(void) {
    static int sLegacy = -1;
    if (sLegacy < 0) {
        const char *e = getenv("MDKR_GRIDMASK");
        sLegacy = (e != NULL && (strcasecmp(e, "off") == 0 || strcasecmp(e, "legacy") == 0));
    }
    return sLegacy;
}

/* Collision-candidate saturation counters, so a headless check can assert on
 * the MECHANISM (the 500-entry list truncating) and not only on the symptom.
 * Fed from generate_collision_candidates(); reported at headless exit as
 * "[COLL] maxCandidates=N truncated=N". Two increments per call, and the
 * high-water mark is what sized the diagnosis. */
static int  s_collMaxCandidates = 0;
static long s_collTruncations   = 0;
/* Forward-declared: mdkr_coll_cap() is defined further down in this file (the
 * MDKR_COLLCAP=<n>|legacy hook), and mdkr_coll_candidates() needs the EFFECTIVE
 * cap for the [COLPEAK] line below so a lowered test cap is reflected there
 * too, not just the ROM's 500. */
int mdkr_coll_cap(int romCap);
void mdkr_coll_candidates(int count, int truncated) {
    if (count > s_collMaxCandidates) {
        s_collMaxCandidates = count;
        /* G4 (docs/open-items/collision.md, wave "boundsweep"): boss levels 41
         * and 54 peak at 416 of 500 with only 84 slots of margin, and the
         * per-run "[COLL] maxCandidates=" summary above only prints once, at
         * headless exit -- it cannot show WHEN in a route the high-water mark
         * moved. This is the per-event counterpart, gated and formatted exactly
         * like the [EVTQ] peak telemetry in platform/audio_event_queue.c
         * (MDKR_TRACE-gated, printed only when a call raises the high-water
         * mark, stable grep-able prefix). It is instrumentation only: it cannot
         * change which candidates are kept, only which peaks get a line in the
         * log. tests/check_collision_headroom.py sweeps the boss levels and
         * reads the "[COLL] maxCandidates=" exit summary (unconditional, so it
         * works without MDKR_TRACE); this trace line is for a human -- or a
         * future finer-grained check -- to see the peak's frame-by-frame
         * approach rather than only its final value. */
        {
            static int s_collTrace = -1;
            if (s_collTrace < 0) {
                const char *t = getenv("MDKR_TRACE");
                s_collTrace = (t != NULL && t[0] != '\0' && t[0] != '0');
            }
            if (s_collTrace) {
                printf("[COLPEAK] candidates new peak %d of %d\n",
                       s_collMaxCandidates, mdkr_coll_cap(500));
            }
        }
    }
    if (truncated) s_collTruncations++;
}
int  mdkr_coll_max_candidates(void) { return s_collMaxCandidates; }
long mdkr_coll_truncations(void)    { return s_collTruncations; }

/* ======================================================================== *
 *  Bounded-write high-water marks -- the ONLY instrument for the
 *  bare-pointer overrun class
 * ======================================================================== *
 * Three functions in game/src/tracks.c write an a-priori unknown number of
 * elements through a caller-supplied pointer that carried no count:
 *
 *   slot 0  get_inside_segment_count_xz()   -> caller's segmentsInside[8]
 *   slot 1  get_inside_segment_count_xyz()  -> caller's inSegs[28]
 *   slot 2  collision_get_y()               -> callers now share
 *           COLLISION_Y_QUERY_CAPACITY (16); the wave builder keeps 30
 *   slot 3  func_800BDC80() triangle fill    -> D_8011C3B8[64] / D_8011C8B8[128]
 *   slot 4  func_800BDC80() height fill      -> its own local spD8[300]
 *
 * They are the whole `bare-pointer` class as enumerated by
 * tools/sweep_bug_shapes.py. Nothing can see the shape from outside: UBSan
 * array-bounds needs an indexed array TYPE and the callee only has a pointer,
 * and tests/check_array_bounds_sweep.py recorded it as its known blind spot.
 * The instrument is the bound parameter itself -- plus these counters, so a
 * headless run reports the high-water mark instead of leaving the slack
 * unmeasured.
 *
 * Reported at headless exit as
 *   [SEGS] xzMax=N/8 xzClamped=N xyzMax=N/28 xyzClamped=N colYMax=N/8 colYClamped=N
 * `Clamped` counts calls where the true count reached the bound, i.e. every call
 * that WOULD have written past the caller's array before the bound existed. It
 * must stay 0 in normal play; a nonzero value is bounded degradation instead of
 * a stack smash.
 */
#define MDKR_BOUND_SLOTS 5
static int  s_boundMax[MDKR_BOUND_SLOTS];
static int  s_boundMin[MDKR_BOUND_SLOTS];
static int  s_boundSlack[MDKR_BOUND_SLOTS];
static long s_boundClamped[MDKR_BOUND_SLOTS];
void mdkr_bound_probe(int slot, int count, int bound) {
    if (slot < 0 || slot >= MDKR_BOUND_SLOTS) return;
    if (count > s_boundMax[slot]) s_boundMax[slot] = count;
    if (s_boundMin[slot] == 0 || bound < s_boundMin[slot]) s_boundMin[slot] = bound;
    /* The number that actually matters. A peak count means nothing on its own
     * when the same callee serves callers with different capacities: measured on
     * boss 40 before the shared COLLISION_Y_QUERY_CAPACITY, collision_get_y
     * peaked at 7 while the smallest caller capacity on that route was 10, so
     * the peak was nowhere near an array's end. Slack is
     * per-CALL, so it cannot be misread that way. */
    if (s_boundSlack[slot] == 0 || (bound - count) < s_boundSlack[slot]) {
        s_boundSlack[slot] = bound - count;
    }
    if (count >= bound) s_boundClamped[slot]++;
}
int  mdkr_bound_slack(int slot) {
    return (slot >= 0 && slot < MDKR_BOUND_SLOTS) ? s_boundSlack[slot] : -1;
}
int  mdkr_bound_max(int slot) {
    return (slot >= 0 && slot < MDKR_BOUND_SLOTS) ? s_boundMax[slot] : -1;
}
/* The smallest capacity any caller passed. It is the denominator in the [SEGS]
 * report on purpose: it is measured, not asserted, so a call site that lost its
 * ARRAY_COUNT shows up as a changed number rather than as nothing at all. */
int  mdkr_bound_min(int slot) {
    return (slot >= 0 && slot < MDKR_BOUND_SLOTS) ? s_boundMin[slot] : -1;
}
long mdkr_bound_clamped(int slot) {
    return (slot >= 0 && slot < MDKR_BOUND_SLOTS) ? s_boundClamped[slot] : -1;
}

/* ======================================================================== *
 *  Shadow-heap fault injection
 * ======================================================================== *
 * MDKR_SHADOW_CAPS=data,tri,vertex lowers (never raises) the three physical
 * heap capacities.  It is a positive-control seam for the transactional shadow
 * builder: a normal run must report zero drops, while a deliberately tiny cap
 * must drop meshes cleanly under ASan/UBSan instead of crossing an allocation.
 */
int mdkr_shadow_cap(int kind, int fallback) {
    static int initialized = 0;
    static int requested[3] = { -1, -1, -1 };
    static const int minimum[3] = { 2, 1, 3 };

    if (!initialized) {
        const char *value = getenv("MDKR_SHADOW_CAPS");
        initialized = 1;
        if (value != NULL && value[0] != '\0') {
            int dataCap, triCap, vtxCap;
            char trailing;
            if (sscanf(value, "%d,%d,%d%c", &dataCap, &triCap, &vtxCap, &trailing) == 3 &&
                dataCap >= minimum[0] && triCap >= minimum[1] && vtxCap >= minimum[2]) {
                requested[0] = dataCap;
                requested[1] = triCap;
                requested[2] = vtxCap;
                fprintf(stderr, "[SHADOW] fault-injection caps requested: data=%d tri=%d vtx=%d\n",
                        dataCap, triCap, vtxCap);
            } else {
                fprintf(stderr,
                        "[SHADOW] ignoring invalid MDKR_SHADOW_CAPS='%s' "
                        "(expected data>=2,tri>=1,vtx>=3)\n",
                        value);
            }
        }
    }

    if (kind < 0 || kind >= 3 || fallback < minimum[kind]) {
        return fallback;
    }
    if (requested[kind] >= minimum[kind] && requested[kind] < fallback) {
        return requested[kind];
    }
    return fallback;
}

/*
 * Positive control for the coplanar-shadow fix. The production default is on;
 * MDKR_SHADOW_DECAL=0 deliberately restores the old ordinary-depth path so the
 * regression harness can prove that it observes real shadow draw groups.
 */
int mdkr_shadow_decal_enabled(void) {
    static int enabled = -1;

    if (enabled < 0) {
        const char *value = getenv("MDKR_SHADOW_DECAL");
        enabled = value == NULL || strcmp(value, "0") != 0;
        if (!enabled) {
            fprintf(stderr,
                    "[SHADOW] diagnostic: decal depth bias disabled "
                    "(MDKR_SHADOW_DECAL=0)\n");
        }
    }
    return enabled;
}

/* ======================================================================== *
 *  MDKR_SEGMARGIN=<n> -- force the segment-overlap lists to overflow
 * ======================================================================== *
 * The two functions above accept a segment whose bounding box is within 4
 * units of the query point. Measured peaks are far below the callers' array
 * sizes (see docs/OPEN_ITEMS.md), so the overrun is UNREACHABLE in normal play
 * and a check that only ran normal play would prove nothing about the bound.
 * This widens that 4-unit margin, which makes more -- eventually all --
 * segments match and drives the count past the bound using the real mechanism
 * (many overlapping segments), not a faked index.
 *
 * With MDKR_SEGBOUND=legacy the bound is not applied, so both arms come from
 * one binary: legacy + margin smashes the caller's stack, bounded + margin
 * exits cleanly. Same contract as MDKR_COLLTEX_FORCE. No-op unless set.
 */
int mdkr_seg_margin(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("MDKR_SEGMARGIN"); s = (e != NULL) ? atoi(e) : 4; }
    return s;
}
int mdkr_segbound_legacy(void) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MDKR_SEGBOUND");
        s = (e != NULL && (strcasecmp(e, "off") == 0 || strcasecmp(e, "legacy") == 0));
    }
    return s;
}

/* ======================================================================== *
 *  MDKR_COLLCAP=<n>|legacy -- reach the collision-candidate cap boundary
 * ======================================================================== *
 * generate_collision_candidates() (game/src/hasm/collision.c) tests its 500-entry
 * cap with `j == MAX_COLLISION_CANDIDATES` after the FACET insert, while the
 * SEGMENT insert at the top of the outer loop advances j with no test at all --
 * faithfully to the ROM. So j can enter that insert at 499, leave at 500, and the
 * equality test is then never true again: the list overruns for every remaining
 * triangle. The measured peak is 416 of 500, so the boundary is not reached and a
 * check that only ran normal play would prove nothing.
 *
 * This returns the effective cap the guards use: MAX_COLLISION_CANDIDATES by
 * default, a lower value so a check can DRIVE the boundary, and INT_MAX for
 * `legacy`, which disables the added guards entirely and leaves only the ROM's own
 * equality test -- i.e. both arms come from one binary, exactly like
 * MDKR_GRIDMASK / MDKR_COLLTEX.
 *
 * The ALLOCATION is deliberately not lowered with the cap (it stays 500 entries in
 * tracks.c). The evidence a check needs is the write INDEX, which
 * "[COLL] maxCandidates" already reports: peak j above the cap means the guard was
 * stepped over. Emulating the boundary without also making the overrun a real heap
 * write keeps the control deterministic. No-op unless set.
 */
/* `romCap` is MAX_COLLISION_CANDIDATES, passed in by the caller rather than
 * duplicated here -- a second copy of the constant is one edit away from being a
 * different constant, and it is the array's size. */
int mdkr_coll_cap(int romCap) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MDKR_COLLCAP");
        if (e == NULL) {
            s = romCap;
        } else if (strcasecmp(e, "off") == 0 || strcasecmp(e, "legacy") == 0) {
            s = 0x7FFFFFFF;
        } else {
            s = atoi(e);
            if (s <= 0 || s > romCap) s = romCap;
        }
    }
    return s;
}

/* ======================================================================== *
 *  MDKR_BOSS_SLOW=1 -- let the human WIN a boss race
 * ======================================================================== *
 * A boss race has exactly two racers, and MDKR_AUTOPILOT drives the human with
 * the same AI the boss uses, so the human reliably finishes SECOND. That makes
 * racer_boss_finish()'s `finishPosition == 1` arm -- the whole win path: the
 * victory jingle, `settings->bosses |= worldBit`, the amulet award and the win
 * cutscene -- unreachable by construction, exactly like the Time-Trial record
 * write was before tests/input_scripts/race_full_3lap_tt.txt existed.
 *
 * This scales the BOSS racer's forward velocity down inside update_tricky (after
 * the shared car physics has run, so nothing else changes), which makes the human
 * finish first. It is the control that lets tests/check_collision_gridmask.py
 * assert the boss result BOTH ways -- win => cutscene 4, loss => cutscene 5 --
 * which is what a "I came first and it told me I lost" report needs in order to
 * be falsifiable. No-op unless set.
 */
int mdkr_boss_slow(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("MDKR_BOSS_SLOW"); s = (e != NULL && atoi(e) != 0); }
    return s;
}
