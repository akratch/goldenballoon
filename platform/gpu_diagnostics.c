#include "gpu_diagnostics.h"

#include <stdio.h>
#include <string.h>

static MdkrGpuInfo s_info;
/* The record is a file static, so it starts zeroed — and a zeroed `selected`
 * is 0, which reads as "candidate zero was chosen" in a block printed before
 * anything initialised it. That block is exactly the one a startup failure
 * produces, so first use configures the record instead of trusting the caller
 * to have reset it. */
static int s_ready;

static void ensure_ready(void) {
    if (!s_ready) {
        mdkr_gpu_info_reset();
    }
}

/* One candidate is one line, and free text is quoted, so any byte that could
 * forge a line or end a field early is replaced on the way in. Bytes at or
 * above 0x80 pass through: adapter names are UTF-8 and mangling one would lose
 * the very detail the record is for. */
static char sanitized(char value) {
    const unsigned char byte = (unsigned char)value;
    if (byte < 0x20u || byte == 0x7Fu) {
        return ' ';
    }
    if (byte == '"') {
        return '\'';
    }
    return value;
}

/* Bounded by the destination size alone, so a source field that a vendor
 * runtime filled to the brim without a terminator is read no further than its
 * own length. Both sides of every copy here are arrays of the same declared
 * size, which is what makes that safe. */
static void copy_field(char *destination, size_t size, const char *source) {
    size_t index = 0u;

    if (destination == NULL || size == 0u) {
        return;
    }
    if (source != NULL) {
        for (; index + 1u < size && source[index] != '\0'; index++) {
            destination[index] = sanitized(source[index]);
        }
    }
    destination[index] = '\0';
}

/* A candidate with nothing to say about why it lost is the report this module
 * exists to prevent. */
static void ensure_reason(char *reason, size_t size) {
    if (reason[0] == '\0') {
        copy_field(reason, size, "no reason recorded");
    }
}

/* Presentation only: the record keeps whatever the backend gave it, and the
 * block says unknown rather than printing a field as empty quotes, which a
 * reader would have to guess at. */
static const char *or_unknown(const char *value) {
    return (value != NULL && value[0] != '\0') ? value : "unknown";
}

void mdkr_gpu_info_reset(void) {
    memset(&s_info, 0, sizeof s_info);
    s_info.count = 0;
    s_info.selected = -1;
    s_info.dropped = 0;
    s_ready = 1;
}

void mdkr_gpu_info_note_candidate(const MdkrGpuCandidate *candidate) {
    MdkrGpuCandidate *slot;

    ensure_ready();
    if (candidate == NULL) {
        return;
    }
    if (s_info.count >= MDKR_GPU_MAX_CANDIDATES) {
        /* Counted, not written. The first candidates are kept because
         * enumeration order is the order the selection policy sees. */
        s_info.dropped++;
        return;
    }

    slot = &s_info.candidates[s_info.count];
    memset(slot, 0, sizeof *slot);
    copy_field(slot->backend, sizeof slot->backend, candidate->backend);
    copy_field(slot->adapter, sizeof slot->adapter, candidate->adapter);
    copy_field(slot->driver, sizeof slot->driver, candidate->driver);
    copy_field(slot->reason, sizeof slot->reason, candidate->reason);
    ensure_reason(slot->reason, sizeof slot->reason);
    slot->vendor_id = candidate->vendor_id;
    slot->device_id = candidate->device_id;
    /* Enumeration never accepts. Only mdkr_gpu_info_select() moves that flag,
     * so the block can never name two winners. */
    slot->accepted = 0;
    s_info.count++;
}

void mdkr_gpu_info_select(int index, const char *reason) {
    int slot;

    ensure_ready();
    if (index < 0 || index >= s_info.count) {
        /* Refused. Writing here on a backend that miscounted would put an
         * adapter nobody enumerated into the record, or put it past the end. */
        return;
    }
    for (slot = 0; slot < s_info.count; slot++) {
        s_info.candidates[slot].accepted = (slot == index) ? 1 : 0;
    }
    if (reason != NULL && reason[0] != '\0') {
        copy_field(s_info.candidates[index].reason,
                   sizeof s_info.candidates[index].reason, reason);
    }
    ensure_reason(s_info.candidates[index].reason,
                  sizeof s_info.candidates[index].reason);
    s_info.selected = index;
}

void mdkr_gpu_info_print(void) {
    int index;

    ensure_ready();

    printf("[GPUINFO] begin\n");
    if (s_info.selected >= 0 && s_info.selected < s_info.count) {
        const MdkrGpuCandidate *chosen = &s_info.candidates[s_info.selected];
        printf("[GPUINFO] selected=%d backend=\"%s\" adapter=\"%s\"\n", s_info.selected,
               or_unknown(chosen->backend), or_unknown(chosen->adapter));
    } else {
        printf("[GPUINFO] selected=none backend=\"none\" adapter=\"none\"\n");
    }
    printf("[GPUINFO] candidates=%d dropped=%d\n", s_info.count, s_info.dropped);
    for (index = 0; index < s_info.count; index++) {
        const MdkrGpuCandidate *candidate = &s_info.candidates[index];
        printf("[GPUINFO] candidate=%d accepted=%d backend=\"%s\" adapter=\"%s\" "
               "vendor=0x%04x device=0x%04x driver=\"%s\" reason=\"%s\"\n",
               index, candidate->accepted ? 1 : 0, or_unknown(candidate->backend),
               or_unknown(candidate->adapter), (unsigned)candidate->vendor_id,
               (unsigned)candidate->device_id, or_unknown(candidate->driver),
               or_unknown(candidate->reason));
    }
    printf("[GPUINFO] end\n");
    /* Flushed, because the run that needs this block most is the one that is
     * about to die before anything else drains stdout. */
    fflush(stdout);
}

const MdkrGpuInfo *mdkr_gpu_info_get(void) {
    ensure_ready();
    return &s_info;
}
