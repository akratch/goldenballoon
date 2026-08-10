#include "adapter_policy.h"

#include <stdio.h>
#include <string.h>

/* Vendor ids whose parts are integrated by construction. NVIDIA is the only
 * vendor treated as discrete on its id alone; the vendors that ship both
 * classes are left unclassified on purpose — see the rule in the header. */
static const uint32_t k_integrated_vendors[] = {
    0x8086u, /* Intel */
    0x106Bu, /* Apple */
    0x5143u, /* Qualcomm */
    0x13B5u, /* ARM */
    0x1010u, /* Imagination */
    0x14E4u  /* Broadcom */
};
#define VENDOR_NVIDIA 0x10DEu

/* ASCII only, and deliberately not tolower(): adapter names arrive as UTF-8
 * from a driver and a locale-sensitive fold would make the same name match
 * differently on two machines, which is the one thing this module must never
 * do. */
static char lower_char(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static int equals_ci(const char *left, const char *right) {
    size_t index;
    for (index = 0u; left[index] != '\0' && right[index] != '\0'; index++) {
        if (lower_char(left[index]) != lower_char(right[index])) {
            return 0;
        }
    }
    return left[index] == '\0' && right[index] == '\0';
}

/* Candidate fields are fixed-size arrays filled by a backend, so every scan is
 * bounded by the field rather than by a terminator that may not be there. */
static size_t field_length(const char *field, size_t size) {
    size_t index = 0u;
    while (index < size && field[index] != '\0') {
        index++;
    }
    return index;
}

static int contains_ci(const char *field, size_t size, const char *needle) {
    const size_t length = field_length(field, size);
    const size_t wanted = strlen(needle);
    size_t       start;
    size_t       offset;

    if (wanted == 0u || wanted > length) {
        return 0;
    }
    for (start = 0u; start + wanted <= length; start++) {
        for (offset = 0u; offset < wanted; offset++) {
            if (lower_char(field[start + offset]) != lower_char(needle[offset])) {
                break;
            }
        }
        if (offset == wanted) {
            return 1;
        }
    }
    return 0;
}

/* One hyphen-separated word of a backend string, so "d3d12" matches
 * "webgpu-d3d12" without "gl" matching every backend that happens to contain
 * those two letters. */
static int backend_word_match(const char *field, size_t size, const char *word) {
    const size_t length = field_length(field, size);
    const size_t wanted = strlen(word);
    size_t       start = 0u;
    size_t       end;
    size_t       offset;

    if (wanted == 0u) {
        return 0;
    }
    while (start <= length) {
        end = start;
        while (end < length && field[end] != '-') {
            end++;
        }
        if (end - start == wanted) {
            for (offset = 0u; offset < wanted; offset++) {
                if (lower_char(field[start + offset]) != lower_char(word[offset])) {
                    break;
                }
            }
            if (offset == wanted) {
                return 1;
            }
        }
        if (end >= length) {
            break;
        }
        start = end + 1u;
    }
    return 0;
}

/* Appends one clause, semicolon-separated, truncating rather than overrunning.
 * Written out instead of snprintf so no clause can be lost to a format the
 * compiler cannot bound. */
static void reason_append(char *reason, size_t size, const char *clause) {
    size_t length;
    size_t index;

    if (reason == NULL || size == 0u || clause == NULL || clause[0] == '\0') {
        return;
    }
    length = field_length(reason, size);
    if (length > 0u) {
        if (length + 3u > size) {
            return;
        }
        reason[length++] = ';';
        reason[length++] = ' ';
        reason[length] = '\0';
    }
    for (index = 0u; clause[index] != '\0' && length + 1u < size; index++) {
        reason[length++] = clause[index];
    }
    reason[length] = '\0';
}

/* Higher wins. UNKNOWN sits between the two named classes in both directions:
 * an adapter we could not classify is never preferred over one we could, and
 * never thrown away either. */
static int class_rank(MdkrAdapterClass adapter_class, int prefer_low_power) {
    switch (adapter_class) {
    case MDKR_ADAPTER_CLASS_DISCRETE:
        return prefer_low_power ? 1 : 3;
    case MDKR_ADAPTER_CLASS_INTEGRATED:
        return prefer_low_power ? 3 : 1;
    case MDKR_ADAPTER_CLASS_UNKNOWN:
    default:
        return 2;
    }
}

MdkrAdapterClass mdkr_adapter_class(const MdkrGpuCandidate *candidate) {
    size_t index;

    if (candidate == NULL) {
        return MDKR_ADAPTER_CLASS_UNKNOWN;
    }
    /* A backend that knows the answer says so in the name; that outranks any
     * inference from an id. */
    if (contains_ci(candidate->adapter, sizeof candidate->adapter, "discrete") ||
        contains_ci(candidate->adapter, sizeof candidate->adapter, "dgpu")) {
        return MDKR_ADAPTER_CLASS_DISCRETE;
    }
    if (contains_ci(candidate->adapter, sizeof candidate->adapter, "integrated") ||
        contains_ci(candidate->adapter, sizeof candidate->adapter, "igpu")) {
        return MDKR_ADAPTER_CLASS_INTEGRATED;
    }
    if (candidate->vendor_id == VENDOR_NVIDIA) {
        return MDKR_ADAPTER_CLASS_DISCRETE;
    }
    for (index = 0u; index < sizeof k_integrated_vendors / sizeof k_integrated_vendors[0];
         index++) {
        if (candidate->vendor_id == k_integrated_vendors[index]) {
            return MDKR_ADAPTER_CLASS_INTEGRATED;
        }
    }
    return MDKR_ADAPTER_CLASS_UNKNOWN;
}

static int candidate_eligible(const MdkrGpuCandidate *candidate, int filtered,
                              const char *backend_preference) {
    return !filtered ||
           backend_word_match(candidate->backend, sizeof candidate->backend,
                              backend_preference);
}

MdkrAdapterChoice mdkr_adapter_policy_choose(const MdkrGpuCandidate *candidates,
                                             int                     count,
                                             const char             *adapter_preference,
                                             const char             *backend_preference) {
    MdkrAdapterChoice choice;
    char              backend_note[MDKR_ADAPTER_REASON_MAX];
    char              miss_note[MDKR_ADAPTER_REASON_MAX];
    char              choice_note[MDKR_ADAPTER_REASON_MAX];
    const char       *adapter_pref = (adapter_preference != NULL) ? adapter_preference : "";
    const char       *backend_pref = (backend_preference != NULL) ? backend_preference : "";
    const char       *class_word;
    int               low_power;
    int               keyword;
    int               backend_auto;
    int               backend_matches = 0;
    int               filtered;
    int               chosen = -1;
    int               best_rank = -1;
    int               index;

    choice.index = MDKR_ADAPTER_POLICY_NONE;
    choice.reason[0] = '\0';
    backend_note[0] = '\0';
    miss_note[0] = '\0';
    choice_note[0] = '\0';

    if (candidates == NULL || count <= 0) {
        /* The one outcome with no index. Everything below this line is
         * guaranteed to find something, which is what lets the caller treat a
         * negative index as "enumeration failed" and nothing else. */
        reason_append(choice.reason, sizeof choice.reason,
                      "no adapters were enumerated, so there was nothing to choose");
        return choice;
    }

    low_power = equals_ci(adapter_pref, "low-power");
    keyword = low_power || adapter_pref[0] == '\0' || equals_ci(adapter_pref, "auto") ||
              equals_ci(adapter_pref, "high-performance");
    class_word = low_power ? "low-power"
                           : (equals_ci(adapter_pref, "high-performance") ? "high-performance"
                                                                          : "auto");

    /* The backend preference narrows the field first. A preference that matches
     * nothing leaves the field whole: no list of legal backend names is kept
     * here, so a typo and a backend this build lacks get the same honest
     * answer. */
    backend_auto = backend_pref[0] == '\0' || equals_ci(backend_pref, "auto");
    if (!backend_auto) {
        for (index = 0; index < count; index++) {
            if (backend_word_match(candidates[index].backend,
                                   sizeof candidates[index].backend, backend_pref)) {
                backend_matches++;
            }
        }
    }
    filtered = !backend_auto && backend_matches > 0;
    if (!backend_auto) {
        if (filtered) {
            (void)snprintf(backend_note, sizeof backend_note,
                           "backend preference \"%.32s\" matched %d of %d", backend_pref,
                           backend_matches, count);
        } else {
            (void)snprintf(backend_note, sizeof backend_note,
                           "backend preference \"%.32s\" matched nothing and was ignored",
                           backend_pref);
        }
    }

    /* Then the adapter preference chooses within what is left. A name beats
     * class outright, and the first match in enumeration order wins so that one
     * saved setting means one thing on every boot. */
    if (!keyword) {
        for (index = 0; index < count; index++) {
            if (!candidate_eligible(&candidates[index], filtered, backend_pref)) {
                continue;
            }
            if (contains_ci(candidates[index].adapter, sizeof candidates[index].adapter,
                            adapter_pref)) {
                chosen = index;
                break;
            }
        }
        if (chosen >= 0) {
            (void)snprintf(choice_note, sizeof choice_note,
                           "adapter name contains \"%.32s\"", adapter_pref);
        } else {
            (void)snprintf(miss_note, sizeof miss_note,
                           "adapter preference \"%.32s\" matched nothing, so auto chose",
                           adapter_pref);
        }
    }

    if (chosen < 0) {
        for (index = 0; index < count; index++) {
            int rank;
            if (!candidate_eligible(&candidates[index], filtered, backend_pref)) {
                continue;
            }
            rank = class_rank(mdkr_adapter_class(&candidates[index]), low_power);
            if (rank > best_rank) {
                best_rank = rank;
                chosen = index;
            }
        }
        /* Reachable only with at least one eligible candidate: count is
         * positive, and the backend filter is armed only when it matched. */
        switch (mdkr_adapter_class(&candidates[chosen])) {
        case MDKR_ADAPTER_CLASS_DISCRETE:
            if (low_power) {
                (void)snprintf(choice_note, sizeof choice_note,
                               "low-power found no integrated adapter and chose a discrete one");
            } else {
                (void)snprintf(choice_note, sizeof choice_note,
                               "%.16s chose the discrete adapter", class_word);
            }
            break;
        case MDKR_ADAPTER_CLASS_INTEGRATED:
            if (low_power) {
                (void)snprintf(choice_note, sizeof choice_note,
                               "low-power chose the integrated adapter");
            } else {
                (void)snprintf(choice_note, sizeof choice_note,
                               "%.16s found no discrete adapter and chose the integrated one",
                               class_word);
            }
            break;
        case MDKR_ADAPTER_CLASS_UNKNOWN:
        default:
            if (low_power) {
                (void)snprintf(choice_note, sizeof choice_note,
                               "low-power found no integrated adapter and chose one of "
                               "unknown class");
            } else {
                (void)snprintf(choice_note, sizeof choice_note,
                               "%.16s found no discrete adapter and chose one of unknown class",
                               class_word);
            }
            break;
        }
    }

    reason_append(choice.reason, sizeof choice.reason, backend_note);
    reason_append(choice.reason, sizeof choice.reason, miss_note);
    reason_append(choice.reason, sizeof choice.reason, choice_note);
    if (choice.reason[0] == '\0') {
        reason_append(choice.reason, sizeof choice.reason, "no reason recorded");
    }
    choice.index = chosen;
    return choice;
}
