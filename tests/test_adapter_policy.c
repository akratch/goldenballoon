/* test_adapter_policy.c — the adapter choice, asserted on a machine with one
 * GPU and in CI with none.
 *
 * This is the test the policy was split out for. The hybrid-graphics laptop
 * that starts on the integrated chip is unreproducible here, so the decision is
 * a pure function over a synthetic candidate list and every case a real machine
 * could present is written down as a table instead of owned as hardware.
 *
 * The fixtures are built the way adapter_policy.h documents the class rule —
 * real PCI vendor ids and real adapter names — not the way the implementation
 * happens to read them. test_the_documented_class_rule() below pins that
 * mapping on its own, so a change to the rule fails there first rather than
 * scattering itself across every behaviour case.
 */
#include "adapter_policy.h"

#include <stdio.h>
#include <string.h>

/* Vendor ids from the rule in adapter_policy.h. */
#define VENDOR_NVIDIA 0x10DEu
#define VENDOR_INTEL  0x8086u
#define VENDOR_AMD    0x1002u
#define VENDOR_APPLE  0x106Bu

static int s_failures;

static void expect(int condition, const char *what) {
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        s_failures++;
    }
}

static void expect_index(const MdkrAdapterChoice *choice, int wanted, const char *what) {
    if (choice->index != wanted) {
        printf("chose index %d, wanted %d, reason: %s\n", choice->index, wanted,
               choice->reason);
    }
    expect(choice->index == wanted, what);
}

static void expect_reason_mentions(const MdkrAdapterChoice *choice, const char *needle,
                                   const char *what) {
    if (strstr(choice->reason, needle) == NULL) {
        printf("reason was: %s\n", choice->reason);
        printf("    wanted it to mention: %s\n", needle);
    }
    expect(strstr(choice->reason, needle) != NULL, what);
}

static MdkrGpuCandidate make(const char *backend, const char *adapter, uint32_t vendor,
                             uint32_t device) {
    MdkrGpuCandidate candidate;
    memset(&candidate, 0, sizeof candidate);
    (void)snprintf(candidate.backend, sizeof candidate.backend, "%s", backend);
    (void)snprintf(candidate.adapter, sizeof candidate.adapter, "%s", adapter);
    (void)snprintf(candidate.driver, sizeof candidate.driver, "%s", "1.0");
    (void)snprintf(candidate.reason, sizeof candidate.reason, "%s", "enumerated");
    candidate.vendor_id = vendor;
    candidate.device_id = device;
    return candidate;
}

/* 0. The class rule, on its own. Every other case builds its fixtures assuming
 * exactly this, so it is asserted before any of them run. */
static void test_the_documented_class_rule(void) {
    MdkrGpuCandidate candidate;

    candidate = make("webgpu-vulkan", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_DISCRETE,
           "NVIDIA's vendor id classes as discrete");

    candidate = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_INTEGRATED,
           "Intel's vendor id classes as integrated");

    candidate = make("webgpu-vulkan", "Apple M2 Pro", VENDOR_APPLE, 0x1u);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_INTEGRATED,
           "so does Apple's");

    /* AMD ships both classes under one vendor id, so it is left unclassified
     * rather than guessed at. The header says so; this pins it. */
    candidate = make("webgpu-vulkan", "AMD Radeon RX 7900 XTX", VENDOR_AMD, 0x744cu);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_UNKNOWN,
           "AMD's vendor id classes as unknown, not as a guess");

    candidate = make("gl", "Some Other GPU", 0x1234u, 0x1u);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_UNKNOWN,
           "a vendor id nobody has heard of classes as unknown");

    /* The name markers are how a backend that knows the answer states it. */
    candidate = make("webgpu-vulkan", "AMD Radeon RX 7900 XTX (discrete)", VENDOR_AMD, 0x744cu);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_DISCRETE,
           "a discrete marker in the name settles an unclassifiable vendor");

    candidate = make("webgpu-vulkan", "NVIDIA Integrated Thing", VENDOR_NVIDIA, 0x1u);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_INTEGRATED,
           "and a marker in the name outranks the vendor id");

    candidate = make("gl", "Vendor iGPU 500", VENDOR_AMD, 0x1u);
    expect(mdkr_adapter_class(&candidate) == MDKR_ADAPTER_CLASS_INTEGRATED,
           "the markers are matched case-insensitively");

    expect(mdkr_adapter_class(NULL) == MDKR_ADAPTER_CLASS_UNKNOWN,
           "a null candidate is unknown, not a crash");
}

/* 1. auto on a hybrid list picks discrete. The whole sprint item, in one line. */
static void test_auto_prefers_discrete(void) {
    MdkrGpuCandidate  list[2];
    MdkrAdapterChoice choice;

    list[0] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    list[1] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);

    choice = mdkr_adapter_policy_choose(list, 2, "auto", "auto");
    expect_index(&choice, 1, "auto picks the discrete adapter over the integrated one");
    expect_reason_mentions(&choice, "discrete", "and says which class it picked");

    /* Enumeration order must not be what decided it. */
    list[0] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    list[1] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    choice = mdkr_adapter_policy_choose(list, 2, "auto", "auto");
    expect_index(&choice, 0, "and picks it whichever way round the list arrives");

    choice = mdkr_adapter_policy_choose(list, 2, "high-performance", "auto");
    expect_index(&choice, 0, "high-performance picks the same adapter, explicitly");

    choice = mdkr_adapter_policy_choose(list, 2, NULL, NULL);
    expect_index(&choice, 0, "null preferences mean auto");
    expect(choice.reason[0] != '\0', "and still come back with a reason");

    choice = mdkr_adapter_policy_choose(list, 2, "", "");
    expect_index(&choice, 0, "so do empty ones");
    expect(choice.reason[0] != '\0', "with a reason too");
}

/* 2. low-power picks integrated even when discrete is present. */
static void test_low_power_prefers_integrated(void) {
    MdkrGpuCandidate  list[2];
    MdkrAdapterChoice choice;

    list[0] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    list[1] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);

    choice = mdkr_adapter_policy_choose(list, 2, "low-power", "auto");
    expect_index(&choice, 1, "low-power picks integrated with a discrete adapter present");
    expect_reason_mentions(&choice, "integrated", "and says so");

    choice = mdkr_adapter_policy_choose(list, 2, "LOW-POWER", "auto");
    expect_index(&choice, 1, "the keyword is matched case-insensitively");

    /* A keyword is never an adapter name, however tempting the name is. */
    list[0] = make("gl", "AutoGraphics Low-Power 3000", VENDOR_INTEL, 0x1u);
    list[1] = make("gl", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    choice = mdkr_adapter_policy_choose(list, 2, "auto", "auto");
    expect_index(&choice, 1,
                 "auto is a keyword, not a substring, even against an adapter named Auto");
}

/* 3. A substring picks the named adapter regardless of class, either case. */
static void test_substring_wins_over_class(void) {
    MdkrGpuCandidate  list[2];
    MdkrAdapterChoice choice;

    list[0] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    list[1] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);

    choice = mdkr_adapter_policy_choose(list, 2, "uhd", "auto");
    expect_index(&choice, 1, "a substring picks the integrated adapter over the discrete one");
    expect_reason_mentions(&choice, "uhd", "and the reason names what matched");

    choice = mdkr_adapter_policy_choose(list, 2, "UHD Graphics", "auto");
    expect_index(&choice, 1, "an upper-case preference matches a mixed-case name");

    list[1] = make("gl", "llvmpipe", 0x0u, 0x0u);
    choice = mdkr_adapter_policy_choose(list, 2, "LLVMPIPE", "auto");
    expect_index(&choice, 1, "and a lower-case name matches an upper-case preference");

    choice = mdkr_adapter_policy_choose(list, 2, "GeForce", "auto");
    expect_index(&choice, 0, "a substring of the discrete adapter picks it as readily");
}

/* 4. A substring matching nothing falls back to auto and says which text it
 * could not find. Mistyping an adapter name must not cost the player a launch,
 * and a silent fallback would leave them believing the setting took. */
static void test_unmatched_substring_falls_back_to_auto(void) {
    MdkrGpuCandidate  list[2];
    MdkrAdapterChoice choice;

    list[0] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    list[1] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);

    choice = mdkr_adapter_policy_choose(list, 2, "Voodoo2", "auto");
    expect_index(&choice, 1, "an unmatched substring still starts, on the auto choice");
    expect_reason_mentions(&choice, "Voodoo2",
                           "and the reason names the text that matched nothing");
    expect_reason_mentions(&choice, "auto", "and says it fell back to auto");

    /* The fallback is auto, not low-power: the class preference is not
     * something an unmatched name gets to change. */
    list[0] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    list[1] = make("webgpu-d3d12", "AMD Radeon RX 7900 XTX", VENDOR_AMD, 0x744cu);
    choice = mdkr_adapter_policy_choose(list, 2, "Voodoo2", "auto");
    expect_index(&choice, 1,
                 "the fallback ranks unknown above integrated, exactly as auto does");
}

/* 5. An empty list. The one outcome with no chosen index, and the reason the
 * sentinel is documented: a caller that subscripted this would read backwards
 * off the front of the array. */
static void test_empty_list_returns_the_sentinel(void) {
    MdkrGpuCandidate  list[1];
    MdkrAdapterChoice choice;

    list[0] = make("gl", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);

    choice = mdkr_adapter_policy_choose(NULL, 0, "auto", "auto");
    expect(choice.index == MDKR_ADAPTER_POLICY_NONE,
           "no candidates at all returns the documented sentinel");
    expect(choice.index < 0, "the sentinel is negative, so no caller can index with it");
    expect(choice.reason[0] != '\0', "and it comes with a reason, like every other outcome");

    choice = mdkr_adapter_policy_choose(list, 0, "auto", "auto");
    expect(choice.index == MDKR_ADAPTER_POLICY_NONE,
           "a real array with a count of zero is the same answer");

    choice = mdkr_adapter_policy_choose(NULL, 3, "auto", "auto");
    expect(choice.index == MDKR_ADAPTER_POLICY_NONE,
           "a null array with a non-zero count is refused rather than dereferenced");

    choice = mdkr_adapter_policy_choose(list, -1, "auto", "auto");
    expect(choice.index == MDKR_ADAPTER_POLICY_NONE, "so is a negative count");

    choice = mdkr_adapter_policy_choose(list, 1, "auto", "auto");
    expect(choice.index == 0, "and one candidate is still a choice");
}

/* 6. The backend preference is applied before the adapter class, asserted
 * across a three-backend list one preference at a time. */
static void test_backend_preference_applies_before_class(void) {
    MdkrGpuCandidate  list[3];
    MdkrAdapterChoice choice;

    /* Only the vulkan candidate is discrete, so any backend preference that
     * lands elsewhere can only have got there by outranking the class. */
    list[0] = make("gl", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    list[1] = make("webgpu-vulkan", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    list[2] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);

    choice = mdkr_adapter_policy_choose(list, 3, "auto", "auto");
    expect_index(&choice, 1, "backend auto leaves the field whole and class decides");

    choice = mdkr_adapter_policy_choose(list, 3, "auto", "gl");
    expect_index(&choice, 0, "backend gl takes the gl candidate, integrated though it is");

    choice = mdkr_adapter_policy_choose(list, 3, "auto", "vulkan");
    expect_index(&choice, 1, "backend vulkan takes the vulkan candidate");

    choice = mdkr_adapter_policy_choose(list, 3, "auto", "d3d12");
    expect_index(&choice, 2, "backend d3d12 takes the d3d12 candidate, not the discrete one");
    expect_reason_mentions(&choice, "d3d12", "and the reason names the backend it honoured");

    choice = mdkr_adapter_policy_choose(list, 3, "auto", "D3D12");
    expect_index(&choice, 2, "the backend preference is matched case-insensitively");

    /* "webgpu" is a word of two of the three backend strings, so the filter
     * narrows to those two and the class rule then decides between them. */
    choice = mdkr_adapter_policy_choose(list, 3, "auto", "webgpu");
    expect_index(&choice, 1, "a backend word matching two candidates leaves class to decide");
    choice = mdkr_adapter_policy_choose(list, 3, "low-power", "webgpu");
    expect_index(&choice, 2, "and low-power then decides between the same two");

    /* A named adapter is chosen inside the backend field, not across it. */
    choice = mdkr_adapter_policy_choose(list, 3, "GeForce", "d3d12");
    expect_index(&choice, 2,
                 "the backend narrows first, so a name outside the field cannot win");
    expect_reason_mentions(&choice, "GeForce", "and the unmatched name is reported");
}

/* 7. auto with only integrated adapters picks one and says so. Reporting "no
 * discrete adapter found" as a failure would strand every laptop that has only
 * ever had one GPU. */
static void test_auto_with_only_integrated_still_chooses(void) {
    MdkrGpuCandidate  list[2];
    MdkrAdapterChoice choice;

    list[0] = make("webgpu-d3d12", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);

    choice = mdkr_adapter_policy_choose(list, 1, "auto", "auto");
    expect_index(&choice, 0, "auto picks the only adapter there is");
    expect_reason_mentions(&choice, "integrated", "and names the class it settled for");

    list[1] = make("gl", "Apple M2 Pro", VENDOR_APPLE, 0x1u);
    choice = mdkr_adapter_policy_choose(list, 2, "auto", "auto");
    expect_index(&choice, 0, "two integrated adapters resolve to the first enumerated");

    /* Unknown sits between the two classes, in both directions. */
    list[0] = make("gl", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    list[1] = make("gl", "AMD Radeon Graphics", VENDOR_AMD, 0x164eu);
    choice = mdkr_adapter_policy_choose(list, 2, "auto", "auto");
    expect_index(&choice, 1, "auto prefers an unclassified adapter to a known integrated one");
    choice = mdkr_adapter_policy_choose(list, 2, "low-power", "auto");
    expect_index(&choice, 0, "and low-power prefers the known integrated one");

    list[0] = make("gl", "AMD Radeon Graphics", VENDOR_AMD, 0x164eu);
    list[1] = make("gl", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    choice = mdkr_adapter_policy_choose(list, 2, "low-power", "auto");
    expect_index(&choice, 0, "low-power prefers an unclassified adapter to a discrete one");
    expect(choice.reason[0] != '\0', "a preference it could not fully honour still explains itself");
}

/* 8. Two adapters matching the same substring: the first enumerated wins, every
 * time. Non-determinism here would make one saved setting mean different things
 * on different boots, which is worse than the setting not working at all. */
static void test_duplicate_substring_matches_are_deterministic(void) {
    MdkrGpuCandidate  list[3];
    MdkrAdapterChoice choice;
    int               repeat;
    int               stable = 1;

    list[0] = make("webgpu-d3d12", "Intel(R) Iris(R) Xe Graphics", VENDOR_INTEL, 0x9a49u);
    list[1] = make("webgpu-d3d12", "AMD Radeon Graphics", VENDOR_AMD, 0x164eu);
    list[2] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);

    choice = mdkr_adapter_policy_choose(list, 3, "graphics", "auto");
    expect_index(&choice, 0,
                 "two adapters matching one substring resolve to the first enumerated");
    for (repeat = 0; repeat < 8; repeat++) {
        const MdkrAdapterChoice again =
            mdkr_adapter_policy_choose(list, 3, "graphics", "auto");
        if (again.index != 0 || strcmp(again.reason, choice.reason) != 0) {
            stable = 0;
        }
    }
    expect(stable, "and resolve to it again on every call, index and reason alike");

    /* Order, and not class or contents: the same three candidates in a
     * different order give a different winner, and the one that moved is the
     * non-matching adapter. Nothing about the matches themselves changed. */
    list[0] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    list[1] = make("webgpu-d3d12", "AMD Radeon Graphics", VENDOR_AMD, 0x164eu);
    list[2] = make("webgpu-d3d12", "Intel(R) Iris(R) Xe Graphics", VENDOR_INTEL, 0x9a49u);
    choice = mdkr_adapter_policy_choose(list, 3, "graphics", "auto");
    expect_index(&choice, 1,
                 "reordering the list moves the winner: enumeration order is the rule");

    /* Identical names are the hybrid-laptop case where both entries are the
     * same model. It still has to be answerable. */
    list[0] = make("webgpu-d3d12", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    list[1] = make("webgpu-vulkan", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    choice = mdkr_adapter_policy_choose(list, 2, "RTX 3070", "auto");
    expect_index(&choice, 0, "two entries for one card resolve to the first enumerated");
}

/* 9. A backend nobody enumerated — a typo, or one this build was not compiled
 * with. Ignored and named, never a refusal to start. */
static void test_unknown_backend_falls_back_to_auto(void) {
    MdkrGpuCandidate  list[2];
    MdkrAdapterChoice choice;

    list[0] = make("gl", "Intel(R) UHD Graphics 630", VENDOR_INTEL, 0x3e9bu);
    list[1] = make("webgpu-vulkan", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);

    choice = mdkr_adapter_policy_choose(list, 2, "auto", "d3d9");
    expect_index(&choice, 1, "an unknown backend name still starts, on the auto choice");
    expect_reason_mentions(&choice, "d3d9", "and the reason names the backend it could not find");

    /* A legal backend that this machine did not enumerate is the same answer:
     * the policy needs no list of legal names to be useful about either. */
    list[1] = make("gl", "NVIDIA GeForce RTX 3070", VENDOR_NVIDIA, 0x2484u);
    choice = mdkr_adapter_policy_choose(list, 2, "auto", "d3d12");
    expect_index(&choice, 1, "a legal backend nobody enumerated is ignored the same way");
    expect_reason_mentions(&choice, "d3d12", "and named the same way");

    /* Both preferences unhonourable at once still yields a running game. */
    choice = mdkr_adapter_policy_choose(list, 2, "Voodoo2", "d3d9");
    expect_index(&choice, 1, "neither preference matching anything is still a choice");
    expect_reason_mentions(&choice, "d3d9", "with the backend miss reported");
    expect_reason_mentions(&choice, "Voodoo2", "and the adapter miss reported alongside it");
    expect(strlen(choice.reason) < (size_t)MDKR_ADAPTER_REASON_MAX,
           "and the whole reason inside its buffer");
}

int main(void) {
    test_the_documented_class_rule();
    test_auto_prefers_discrete();
    test_low_power_prefers_integrated();
    test_substring_wins_over_class();
    test_unmatched_substring_falls_back_to_auto();
    test_empty_list_returns_the_sentinel();
    test_backend_preference_applies_before_class();
    test_auto_with_only_integrated_still_chooses();
    test_duplicate_substring_matches_are_deterministic();
    test_unknown_backend_falls_back_to_auto();
    if (s_failures != 0) {
        printf("FAILURES: %d\n", s_failures);
        return 1;
    }
    printf("all adapter policy assertions passed\n");
    return 0;
}
