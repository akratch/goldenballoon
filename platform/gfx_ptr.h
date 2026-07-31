/**
 * gfx_ptr.h — 64-bit pointer lookup table for GBI display list addresses.
 *
 * On 64-bit PC, GBI macros truncate pointers to 32-bit (unsigned int).
 * This table records full 64-bit pointers so the GBI translator can
 * resolve them. Game code calls gfx_ptr_store() via overridden macros;
 * the translator calls gfx_ptr_resolve() when reading addresses from
 * display list commands.
 *
 * Open addressing over the whole table with a per-slot state byte
 * (EMPTY/OCCUPIED/TOMBSTONE). Insert is lossless — it never silently evicts a
 * live mapping — and range invalidation tombstones (rather than zeroes) slots so
 * a deletion mid-probe-chain can never hide a still-live mapping (AUDIT-0009).
 */
#ifndef GFX_PTR_H
#define GFX_PTR_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef MDKR_TRAP_SIGNEXT
#include <stdio.h>
#endif

#define GFX_PTR_TABLE_BITS 16
#define GFX_PTR_TABLE_SIZE (1 << GFX_PTR_TABLE_BITS)
#define GFX_PTR_TABLE_MASK (GFX_PTR_TABLE_SIZE - 1)
#define GFX_PTR_PROBE_MAX  4   /* legacy warn threshold; see gfx_ptr_max_probe */

/* Slot occupancy for gfx_ptr_state[] (open addressing with tombstones). */
#define GFX_PTR_EMPTY     0u
#define GFX_PTR_OCCUPIED  1u
#define GFX_PTR_TOMBSTONE 2u

/* Segment shadow table — full 64-bit segment base addresses */
extern uintptr_t gfx_segment_table[16];

/* Pointer table — open-addressed hash of truncated→full mappings.
 * gfx_ptr_state[] records each slot's EMPTY/OCCUPIED/TOMBSTONE status so a
 * deletion mid-probe-chain can tombstone (not zero) the slot, keeping later
 * live mappings reachable, and so store never silently evicts (AUDIT-0009).
 *
 * On ILP32/wasm32 every DL host pointer can register here
 * (GFX_DL_REGISTER_PTR / osVirtualToPhysical). Stage-generation cleanup and
 * texture-arena range invalidation bound those entries just as on LP64; the
 * session-long high-water/failure counters remain the browser-soak witness. */
extern uint32_t  gfx_ptr_keys[GFX_PTR_TABLE_SIZE];
extern uintptr_t gfx_ptr_vals[GFX_PTR_TABLE_SIZE];
extern uint8_t   gfx_ptr_state[GFX_PTR_TABLE_SIZE];
/* Raw/global DL pointers can be registered while a queued task is being built,
 * before a level teardown clears stage-local mappings. Epoch-stamped slots get
 * one teardown of grace and must be observed again in the next stage to survive
 * another, preventing both the queued-task race and permanent dynamic growth. */
extern uint32_t  gfx_ptr_epoch[GFX_PTR_TABLE_SIZE];
extern uint32_t  gfx_ptr_epoch_current;

/* Render-health telemetry (AUDIT-0009); read by diagnostics, never gates sim. */
extern uint32_t  gfx_ptr_ambiguous;   /* two live full ptrs shared one low-32 */
extern uint32_t  gfx_ptr_full_fails;  /* insert refused: table 100% occupied */
extern uint32_t  gfx_ptr_max_probe;   /* longest probe distance ever walked */
extern uint32_t  gfx_ptr_live;        /* current OCCUPIED slot count */
extern uint32_t  gfx_ptr_high_water;  /* session-long OCCUPIED high-water */

/* PERF-052: per-thread registry-write opt-out. The registry is MAIN-THREAD
 * state: only the main thread submits display lists and resolves tokens. The
 * opt-in audio-synth worker (audio_pc.c) reaches gfx_ptr_store anyway, because
 * the port's alAudioFrame builds its VESTIGIAL Acmd list (never executed —
 * audi_port.c discards it) through osVirtualToPhysical, whose store side-effect
 * exists for the renderer's benefit only. Those worker stores would race the
 * render path's probes, so the worker sets this thread-local to 1 once at
 * thread start and gfx_ptr_store becomes a no-op on that thread.
 *
 * Thread-local, not a global flag, so the MAIN thread's behavior is untouched
 * by construction (its copy is always 0 — the store path is bit-for-bit the
 * historical one, no lock, no atomic). Definition lives with the other
 * registry globals (gfx_pc.c; test stub in test_gfx_ptr_registry.c). */
#if defined(_MSC_VER)
#define GFX_PTR_TLS __declspec(thread)
#else
#define GFX_PTR_TLS __thread
#endif
extern GFX_PTR_TLS int gfx_ptr_store_suppress;

/* Record an insert's probe-walk length into the cumulative max-probe high-water
 * mark. Hoisted out of the insert body so EVERY insert path updates it: both the
 * common EMPTY-terminated path and the full-table-scan path (a tombstone reused
 * with no EMPTY terminator anywhere). That scan path is reached precisely when
 * the table is aging/saturated, so leaving it un-recorded under-reported the
 * probe depth exactly when it was worst (WEB-037 / AUDIT-0009). */
static inline void gfx_ptr_note_probe(uint32_t walked) {
    if (walked > gfx_ptr_max_probe) {
        gfx_ptr_max_probe = walked;
    }
}

static inline void gfx_ptr_store_lifetime(const void *ptr, int persistent) {
    uintptr_t full = (uintptr_t)ptr;
    uint32_t key = (uint32_t)full;

    /* Robustness net (char-select SIGSEGV class): a value whose high 32 bits are
     * all ones is a 32-bit token that some upstream LP64 pointer truncation
     * sign-extended back to 64-bit — never a real host pointer (user-space
     * mappings never live at 0xffffffff........, and the arena's high bits are
     * 0x4-0x7). Registering it would let a later resolve hand the wild pointer
     * straight to the DL consumers, which then fault on deref. Refuse to store
     * it. The true fix is always at the truncation site (see tracks.c
     * render_level_segment); this guard just guarantees the registry can never
     * become the delivery vehicle for a wild pointer regardless of input. */
#if UINTPTR_MAX > UINT32_MAX
    if ((full >> 32) == UINT32_MAX) {
#ifdef MDKR_TRAP_SIGNEXT
        fprintf(stderr, "[TRAP] gfx_ptr_store sign-extended ptr %p (key=%08x)\n",
                ptr, key);
        fflush(stderr);
        __builtin_trap();
#endif
        return;
    }
#endif

    /* PERF-052: the audio-synth worker's stores serve a never-executed Acmd
     * list; suppress them so they cannot race the render path (see the
     * gfx_ptr_store_suppress comment above). Always 0 on the main thread. */
    if (gfx_ptr_store_suppress) {
        return;
    }
    uint32_t base_idx = ((key >> 2) ^ (key >> 16)) & GFX_PTR_TABLE_MASK;
    uint32_t free_idx = 0;
    int have_free = 0;
    uint32_t p;

    /* Open-addressing probe over the whole table. Reuse the first EMPTY or
     * TOMBSTONE slot, but keep scanning until an EMPTY terminates the chain so
     * an already-live key is always found first (lossless -- never evicts). */
    for (p = 0; p < GFX_PTR_TABLE_SIZE; p++) {
        uint32_t idx = (base_idx + p) & GFX_PTR_TABLE_MASK;
        uint8_t st = gfx_ptr_state[idx];
        if (st == GFX_PTR_OCCUPIED) {
            if (gfx_ptr_keys[idx] == key) {
                if (gfx_ptr_vals[idx] == full) {
                    if (persistent) {
                        gfx_ptr_epoch[idx] = gfx_ptr_epoch_current;
                    }
                    return;                 /* duplicate mapping: stable no-op */
                }
                /* Two live full pointers share one low-32 token: ambiguous.
                 * Fail closed -- keep the incumbent, never overwrite. */
                gfx_ptr_ambiguous++;
                return;
            }
            continue;                       /* different key: keep probing */
        }
        if (!have_free) {
            free_idx = idx;                 /* first reusable (EMPTY/TOMBSTONE) slot */
            have_free = 1;
        }
        if (st == GFX_PTR_EMPTY) {
            gfx_ptr_note_probe(p);          /* slots walked to the free terminator */
            gfx_ptr_keys[free_idx] = key;
            gfx_ptr_vals[free_idx] = full;
            gfx_ptr_state[free_idx] = GFX_PTR_OCCUPIED;
            gfx_ptr_epoch[free_idx] =
                persistent ? gfx_ptr_epoch_current : 0;
            gfx_ptr_live++;
            if (gfx_ptr_live > gfx_ptr_high_water) {
                gfx_ptr_high_water = gfx_ptr_live;
            }
            return;
        }
        /* TOMBSTONE: remembered as free_idx above; keep scanning for a live match. */
    }

    /* Scanned every slot with no EMPTY terminator. */
    if (have_free) {
        /* Walked the whole table (no EMPTY terminates the chain): a later resolve
         * miss on this base would walk it in full too, so record the maximum. */
        gfx_ptr_note_probe(GFX_PTR_TABLE_SIZE - 1u);
        gfx_ptr_keys[free_idx] = key;
        gfx_ptr_vals[free_idx] = full;
        gfx_ptr_state[free_idx] = GFX_PTR_OCCUPIED;
        gfx_ptr_epoch[free_idx] =
            persistent ? gfx_ptr_epoch_current : 0;
        gfx_ptr_live++;
        if (gfx_ptr_live > gfx_ptr_high_water) {
            gfx_ptr_high_water = gfx_ptr_live;
        }
        return;
    }
    gfx_ptr_full_fails++;                    /* table full: refuse, no eviction */
}

static inline void gfx_ptr_store(const void *ptr) {
    gfx_ptr_store_lifetime(ptr, 0);
}

static inline void gfx_ptr_store_persistent(const void *ptr) {
    gfx_ptr_store_lifetime(ptr, 1);
}

/* Drop every mapping whose full pointer falls in [lo, hi). Call before freeing
 * a heap region that pointers were stored from (e.g. a retired texture-arena
 * chunk) so a later resolve can never hand back freed memory. A deleted slot
 * becomes a TOMBSTONE, not EMPTY, so it never truncates a probe chain and hides
 * a still-live mapping stored later in the same chain (AUDIT-0009). */
static inline void gfx_ptr_invalidate_range(uintptr_t lo, uintptr_t hi) {
    uint32_t i;
    if (hi <= lo) {
        return;
    }
    for (i = 0; i < GFX_PTR_TABLE_SIZE; i++) {
        if (gfx_ptr_state[i] == GFX_PTR_OCCUPIED &&
            gfx_ptr_vals[i] >= lo && gfx_ptr_vals[i] < hi) {
            gfx_ptr_keys[i] = 0;
            gfx_ptr_vals[i] = 0;
            gfx_ptr_state[i] = GFX_PTR_TOMBSTONE;
            gfx_ptr_epoch[i] = 0;
            if (gfx_ptr_live > 0) {
                gfx_ptr_live--;
            }
        }
    }
}

/* Full occupancy/epoch reset, used for boot-equivalent unit/reset paths.
 * Production stage teardown uses gfx_ptr_clear_transient() below so pointers
 * already encoded in queued graphics work remain resolvable for one boundary.
 * Session-long failure/probe/high-water telemetry is deliberately preserved. */
static inline void gfx_ptr_clear(void) {
    memset(gfx_ptr_state, GFX_PTR_EMPTY, sizeof(gfx_ptr_state));
    memset(gfx_ptr_keys, 0, sizeof(gfx_ptr_keys));
    memset(gfx_ptr_vals, 0, sizeof(gfx_ptr_vals));
    memset(gfx_ptr_epoch, 0, sizeof(gfx_ptr_epoch));
    gfx_ptr_live = 0;
    gfx_ptr_epoch_current = 1;
}

/*
 * Drop stage-local mappings while retaining raw/global pointers that may
 * already be referenced by a queued display list. A full clear here used to
 * race level transitions: rdp_init()/rsp_init() registered dRdpInit/dRspInit,
 * level_free() erased the registry, and the queued G_DL token then fell through
 * to an unrelated live segment with the same top nibble. The interpreter walked
 * nondeterministic arena bytes as commands until its boundary guard fired.
 *
 * Rebuild instead of leaving tombstones so repeated level transitions cannot
 * lengthen miss probes. The scratch is process-static to avoid a 512 KiB stack
 * allocation at teardown.
 */
static inline void gfx_ptr_clear_transient(void) {
    static uintptr_t retained[GFX_PTR_TABLE_SIZE];
    uint32_t retained_count = 0;
    uint32_t i;
    for (i = 0; i < GFX_PTR_TABLE_SIZE; i++) {
        if (gfx_ptr_state[i] == GFX_PTR_OCCUPIED &&
            gfx_ptr_epoch[i] == gfx_ptr_epoch_current) {
            retained[retained_count++] = gfx_ptr_vals[i];
        }
    }
    memset(gfx_ptr_state, GFX_PTR_EMPTY, sizeof(gfx_ptr_state));
    memset(gfx_ptr_keys, 0, sizeof(gfx_ptr_keys));
    memset(gfx_ptr_vals, 0, sizeof(gfx_ptr_vals));
    memset(gfx_ptr_epoch, 0, sizeof(gfx_ptr_epoch));
    gfx_ptr_live = 0;
    for (i = 0; i < retained_count; i++) {
        gfx_ptr_store_persistent((const void *)retained[i]);
    }
    gfx_ptr_epoch_current =
        gfx_ptr_epoch_current == UINT32_MAX ? 1 : gfx_ptr_epoch_current + 1;
}

static inline void *gfx_ptr_resolve(uint32_t key) {
    if (key == 0) return NULL;
    uint32_t base_idx = ((key >> 2) ^ (key >> 16)) & GFX_PTR_TABLE_MASK;
    uint32_t p;
    for (p = 0; p < GFX_PTR_TABLE_SIZE; p++) {
        uint32_t idx = (base_idx + p) & GFX_PTR_TABLE_MASK;
        uint8_t st = gfx_ptr_state[idx];
        if (st == GFX_PTR_EMPTY) {
            break;                          /* end of chain: genuine miss */
        }
        if (st == GFX_PTR_OCCUPIED && gfx_ptr_keys[idx] == key) {
            return (void *)gfx_ptr_vals[idx];
        }
        /* TOMBSTONE or non-matching OCCUPIED: keep probing. */
    }
    return NULL;
}

/**
 * Resolve a segment address: (segment << 24) | offset
 * If top nibble is 0x01-0x0F, it's a segment address.
 * Otherwise try the pointer table.
 */
static inline void *gfx_resolve_addr(uint32_t addr) {
    if (addr == 0) return NULL;
    uint32_t seg = (addr >> 24) & 0x0F;
    if (seg > 0 && gfx_segment_table[seg] != 0) {
        uint32_t offset = addr & 0x00FFFFFF;
        return (void *)(gfx_segment_table[seg] + offset);
    }
    void *resolved = gfx_ptr_resolve(addr);
    if (resolved) return resolved;
    /* Cannot resolve — return NULL instead of casting raw N64 address.
     * Callers must check for NULL. */
    return NULL;
}

/* ILP32 (wasm32) display-list pointer registration.
 *
 * On LP64 the GBI translator distinguishes a runtime host pointer from an N64
 * segment token by the pointer's high 32 bits (a token is a bare 32-bit value).
 * On a 32-bit target both are 32-bit and that test collapses, so every host
 * pointer that is written into a DL word must be recorded in the pointer
 * registry; the translator then treats a registry hit as a host pointer and a
 * miss (with a live segment nibble) as a segment token (see seg_addr /
 * gfx_resolve_texture_image_token). Most DL-word pointers already register via
 * osVirtualToPhysical(); this macro covers the remaining hand-built DL sites
 * that assign words.w1 directly.
 *
 * The >0x00FFFFFF guard mirrors osVirtualToPhysical(): values at or below the
 * 24-bit N64 segment range are low host pointers (nibble 0, e.g. static data)
 * that the translator resolves directly, so they need no registry entry.
 *
 * No-op on LP64: the native path stays byte-identical. */
#if UINTPTR_MAX == 0xffffffffu
#define GFX_DL_REGISTER_PTR(v)                                          \
    do {                                                               \
        uintptr_t gfx_dl_reg_p_ = (uintptr_t)(v);                      \
        if (gfx_dl_reg_p_ > 0x00FFFFFFu) {                             \
            gfx_ptr_store((const void *)gfx_dl_reg_p_);                \
        }                                                              \
    } while (0)
#else
#define GFX_DL_REGISTER_PTR(v) ((void)0)
#endif

#endif /* GFX_PTR_H */
