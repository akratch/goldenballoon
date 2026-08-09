/* gpu_diagnostics.h — what adapter we chose, and why we rejected the others.
 *
 * Three of this project's open platform items are the same item: something did
 * not work on hardware the maintainers do not have, and the report could not
 * say what the hardware was. This record exists so that the next such report
 * arrives complete.
 *
 * The trap is a graphics failure that reports only itself. A player whose game
 * will not start can copy one [GPUINFO] block and the reader learns the
 * backend, the adapter, its vendor and device ids, the driver string, and the
 * reason every other candidate lost — without owning the machine. That only
 * works if the block survives the failure, so the record is filled in while
 * adapters are being enumerated, not once a device exists, and printing it
 * writes to stdout and touches nothing else. It is safe before a window, before
 * a backend, and before this module's own reset().
 *
 * Every candidate carries a non-empty reason. A rejected adapter with no stated
 * reason is the failure this module exists to prevent, so an empty one is
 * replaced with an explicit placeholder rather than printed as silence.
 *
 * Not thread safe, and it does not need to be: the record is written during
 * single-threaded backend selection at startup and read afterwards.
 */
#ifndef MDKR64_GPU_DIAGNOSTICS_H
#define MDKR64_GPU_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_GPU_MAX_CANDIDATES 8
#define MDKR_GPU_STR_MAX        192

typedef struct MdkrGpuCandidate {
    char backend[32];       /* "webgpu-d3d12", "webgpu-vulkan", "gl" */
    char adapter[MDKR_GPU_STR_MAX];
    char driver[MDKR_GPU_STR_MAX];
    uint32_t vendor_id, device_id;
    int  accepted;          /* 1 selected, 0 rejected */
    char reason[MDKR_GPU_STR_MAX]; /* never empty */
} MdkrGpuCandidate;

typedef struct MdkrGpuInfo {
    MdkrGpuCandidate candidates[MDKR_GPU_MAX_CANDIDATES];
    int              count;
    int              selected;  /* index, or -1 */
    /* Candidates refused for want of room. Reported in the block: a list that
     * was silently truncated would read as the whole machine. */
    int              dropped;
} MdkrGpuInfo;

/* Empties the record. A backend that retries enumeration calls this first. */
void mdkr_gpu_info_reset(void);

/* Records one enumerated adapter, in enumeration order. Copies out of the
 * caller's struct, bounding every field by its own size, so a driver string
 * that arrived without a terminator cannot take the copy off the end. A null
 * candidate is ignored. Past MDKR_GPU_MAX_CANDIDATES the surplus is counted in
 * MdkrGpuInfo::dropped and nothing is written; the earliest candidates are kept
 * because enumeration order is the order the selection policy sees. */
void mdkr_gpu_info_note_candidate(const MdkrGpuCandidate *candidate);

/* Marks candidate `index` as the one in use and records why. Exactly one
 * candidate is ever accepted; selecting again moves the flag. An index outside
 * [0, count) is refused — `selected` stays -1 and nothing is written — because
 * a backend that miscounted must not be able to make the record describe an
 * adapter nobody enumerated. An empty or null reason leaves the reason already
 * recorded in place. */
void mdkr_gpu_info_select(int index, const char *reason);

/* Emits the `[GPUINFO]` block. Safe to call before a window exists.
 *
 * Every line carries the marker so the block survives interleaving with the
 * rest of a run's output:
 *
 *   [GPUINFO] begin
 *   [GPUINFO] selected=<index|none> backend="<name>" adapter="<name>"
 *   [GPUINFO] candidates=<n> dropped=<n>
 *   [GPUINFO] candidate=<i> accepted=<0|1> backend="…" adapter="…"
 *             vendor=0x<hex> device=0x<hex> driver="…" reason="…"
 *   [GPUINFO] end
 *
 * One candidate is one line. Free text is quoted so a reader knows where a
 * field ends, and control characters and double quotes are replaced on the way
 * in, so no adapter name can forge a line or end a field early. A field the
 * backend left empty prints as unknown rather than as empty quotes. */
void mdkr_gpu_info_print(void);

/* The live record. Never null. */
const MdkrGpuInfo *mdkr_gpu_info_get(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_GPU_DIAGNOSTICS_H */
