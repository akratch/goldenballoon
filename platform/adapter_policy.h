/* adapter_policy.h — which adapter to use, decided from the record alone.
 *
 * The hybrid-graphics laptop that launches on the integrated chip is a bug the
 * maintainers cannot reproduce, because nobody here has one. So the decision is
 * kept out of the backend entirely and made here, over a plain MdkrGpuCandidate
 * list, with no Dawn, WebGPU or GL type anywhere in the signature. That is the
 * whole point of the split: the policy is exhaustively testable on a machine
 * with one GPU, and in hosted CI with none.
 *
 * This module decides and nothing else. It opens no device, and it never fails
 * a launch: a preference that cannot be honoured is reported in the reason and
 * ignored, because a player who mistyped an adapter name should still get a
 * game. The only outcome with no chosen index is an empty candidate list, and
 * that one is a sentinel the caller must test for — see below.
 *
 * Validating the config strings is not this module's job. Anything at all can
 * be passed here; unrecognised text is treated as an adapter-name substring and
 * an unmatched preference falls back to auto with a reason that names it.
 *
 * ── How the class of an adapter is decided ───────────────────────────────────
 *
 * From the record, in this order:
 *
 *  1. The adapter name, matched case-insensitively. A name containing
 *     "integrated" or "igpu" is INTEGRATED; one containing "discrete" or "dgpu"
 *     is DISCRETE. This exists so a backend that knows the answer can state it:
 *     Dawn reports an adapter type, and appending " (discrete)" to the name it
 *     records says so without a new field.
 *  2. Otherwise the PCI vendor id:
 *       0x10DE NVIDIA                                        -> DISCRETE
 *       0x8086 Intel, 0x106B Apple, 0x5143 Qualcomm,
 *       0x13B5 ARM, 0x1010 Imagination, 0x14E4 Broadcom      -> INTEGRATED
 *  3. Otherwise UNKNOWN.
 *
 * Device ids are deliberately not consulted. Per-vendor device tables are stale
 * the day a card ships, and a stale table would misclassify silently, which is
 * worse than declining to classify.
 *
 * Two vendors are therefore known to be classified imprecisely: AMD (0x1002)
 * ships both classes under one id and comes out UNKNOWN, and an Intel discrete
 * card comes out INTEGRATED by vendor id. UNKNOWN is ranked between the other
 * two, so an unclassified adapter is never preferred over a known-good one and
 * never rejected outright, and in both cases the player's remedy is an adapter
 * substring, which overrides class entirely.
 *
 * ── How a choice is made ─────────────────────────────────────────────────────
 *
 * The backend preference is applied first and narrows the field; the adapter
 * preference then chooses within what is left.
 *
 *  - Backend preference: "auto" or empty keeps every candidate. Anything else
 *    is matched case-insensitively against the hyphen-separated words of each
 *    candidate's backend string, so "d3d12" matches "webgpu-d3d12" and "gl"
 *    matches "gl". If it matches no candidate the field is left whole and the
 *    reason says the preference was ignored — which covers a typo and a backend
 *    this build was not compiled with, without needing a list of legal names.
 *  - Adapter preference: "auto", "high-performance" and "low-power" are
 *    keywords, matched case-insensitively, and are never treated as names.
 *    Empty or null means "auto". "low-power" ranks INTEGRATED first, the other
 *    two rank DISCRETE first, and UNKNOWN sits between in both.
 *  - Any other text is a case-insensitive substring of the adapter name, and
 *    wins regardless of class. If it matches nothing the choice falls back to
 *    auto and the reason names the unmatched text.
 *  - Ties are broken by enumeration order, always. Two adapters matching the
 *    same substring must resolve the same way on every boot, or a player's
 *    setting means different things on different days.
 */
#ifndef MDKR64_ADAPTER_POLICY_H
#define MDKR64_ADAPTER_POLICY_H

#include "gpu_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MdkrAdapterChoice::index when no choice was possible. It is not an index:
 * test for it, or for a negative index, before subscripting anything. The only
 * way to get it is an empty or null candidate list. */
#define MDKR_ADAPTER_POLICY_NONE (-1)

/* Sized to drop straight into MdkrGpuCandidate::reason and
 * mdkr_gpu_info_select() without a second length to keep in step. */
#define MDKR_ADAPTER_REASON_MAX MDKR_GPU_STR_MAX

typedef enum MdkrAdapterClass {
    MDKR_ADAPTER_CLASS_UNKNOWN = 0,
    MDKR_ADAPTER_CLASS_INTEGRATED,
    MDKR_ADAPTER_CLASS_DISCRETE
} MdkrAdapterClass;

typedef struct MdkrAdapterChoice {
    int  index;                          /* or MDKR_ADAPTER_POLICY_NONE */
    char reason[MDKR_ADAPTER_REASON_MAX]; /* never empty */
} MdkrAdapterChoice;

/* The class rule above, applied to one candidate. Null is UNKNOWN. */
MdkrAdapterClass mdkr_adapter_class(const MdkrGpuCandidate *candidate);

/* Chooses one of `count` candidates. Pure: reads only its arguments, writes
 * only its return value, allocates nothing. `count` is the caller's array
 * length, not MDKR_GPU_MAX_CANDIDATES. Either preference may be null, which
 * means auto. The reason is never empty, whatever the outcome, because a
 * selection with no stated reason is what put this module here. Preference text
 * echoed into the reason is abbreviated to keep the whole reason inside
 * MDKR_ADAPTER_REASON_MAX. */
MdkrAdapterChoice mdkr_adapter_policy_choose(const MdkrGpuCandidate *candidates,
                                             int                     count,
                                             const char             *adapter_preference,
                                             const char             *backend_preference);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ADAPTER_POLICY_H */
