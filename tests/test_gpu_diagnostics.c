/* test_gpu_diagnostics.c — the record that makes a graphics bug report
 * complete, asserted with no GPU, no window and no backend.
 *
 * Every case here is a failure path, because the run that needs the record most
 * is the run that never got a device. So the block is asserted line for line:
 * a later gate will parse these lines out of a headless run, and a shape that
 * drifted silently would take the gate with it.
 *
 * The record is a file static, so the cases share it. Each one resets first,
 * except the very first, which deliberately does not — see its comment.
 */
#include "gpu_diagnostics.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
static int  dup_fd(int fd) { return _dup(fd); }
static int  replace_fd(int source, int target) { return _dup2(source, target); }
static void close_fd(int fd) { if (fd >= 0) { _close(fd); } }
#else
#include <unistd.h>
static int  dup_fd(int fd) { return dup(fd); }
static int  replace_fd(int source, int target) { return dup2(source, target); }
static void close_fd(int fd) { if (fd >= 0) { (void)close(fd); } }
#endif

static int s_failures;

static void expect(int condition, const char *what) {
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        s_failures++;
    }
}

static FILE *s_capture;
static int   s_saved_stdout = -1;

static int capture_begin(void) {
    s_capture = tmpfile();
    if (s_capture == NULL) {
        return 0;
    }
    fflush(stdout);
    s_saved_stdout = dup_fd(fileno(stdout));
    if (s_saved_stdout < 0 || replace_fd(fileno(s_capture), fileno(stdout)) < 0) {
        close_fd(s_saved_stdout);
        s_saved_stdout = -1;
        (void)fclose(s_capture);
        s_capture = NULL;
        return 0;
    }
    return 1;
}

static void capture_end(char *out, size_t out_size) {
    size_t read_bytes;
    fflush(stdout);
    (void)replace_fd(s_saved_stdout, fileno(stdout));
    close_fd(s_saved_stdout);
    s_saved_stdout = -1;
    rewind(s_capture);
    read_bytes = fread(out, 1u, out_size - 1u, s_capture);
    out[read_bytes] = '\0';
    (void)fclose(s_capture);
    s_capture = NULL;
}

/* Runs the printer with stdout redirected. On a capture failure the buffer
 * comes back empty, which fails the shape assertions rather than passing. */
static void capture_print(char *out, size_t out_size) {
    if (!capture_begin()) {
        out[0] = '\0';
        return;
    }
    mdkr_gpu_info_print();
    capture_end(out, out_size);
}

static int occurrences(const char *text, const char *needle) {
    const char  *cursor = text;
    const size_t length = strlen(needle);
    int          found = 0;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        found++;
        cursor += length;
    }
    return found;
}

static int line_count(const char *text) {
    int found = 0;
    size_t index;
    for (index = 0u; text[index] != '\0'; index++) {
        if (text[index] == '\n') {
            found++;
        }
    }
    return found;
}

/* Line `wanted` of `text`, without its newline. Absent lines come back empty,
 * so a block that is short fails the comparison instead of reading past it. */
static void line_at(const char *text, int wanted, char *out, size_t out_size) {
    const char *cursor = text;
    const char *end;
    size_t      length;
    int         index;

    out[0] = '\0';
    for (index = 0; index < wanted; index++) {
        cursor = strchr(cursor, '\n');
        if (cursor == NULL) {
            return;
        }
        cursor++;
    }
    end = strchr(cursor, '\n');
    length = (end != NULL) ? (size_t)(end - cursor) : strlen(cursor);
    if (length >= out_size) {
        length = out_size - 1u;
    }
    memcpy(out, cursor, length);
    out[length] = '\0';
}

static int line_is(const char *text, int wanted, const char *expected) {
    char line[512];
    line_at(text, wanted, line, sizeof line);
    if (strcmp(line, expected) != 0) {
        printf("line %d was: %s\n", wanted, line);
        printf("     wanted: %s\n", expected);
        return 0;
    }
    return 1;
}

static MdkrGpuCandidate make_candidate(const char *backend, const char *adapter,
                                       const char *driver, uint32_t vendor,
                                       uint32_t device, const char *reason) {
    MdkrGpuCandidate candidate;
    memset(&candidate, 0, sizeof candidate);
    (void)snprintf(candidate.backend, sizeof candidate.backend, "%s", backend);
    (void)snprintf(candidate.adapter, sizeof candidate.adapter, "%s", adapter);
    (void)snprintf(candidate.driver, sizeof candidate.driver, "%s", driver);
    candidate.vendor_id = vendor;
    candidate.device_id = device;
    (void)snprintf(candidate.reason, sizeof candidate.reason, "%s", reason);
    return candidate;
}

static void note(const char *backend, const char *adapter, const char *driver,
                 uint32_t vendor, uint32_t device, const char *reason) {
    const MdkrGpuCandidate candidate =
        make_candidate(backend, adapter, driver, vendor, device, reason);
    mdkr_gpu_info_note_candidate(&candidate);
}

/* 1. The startup-failure path. No window, no device, and — because this case
 * runs before any other — no mdkr_gpu_info_reset() either. A record that has
 * never been initialised must still print a complete block whose selected
 * index reads as none, not as candidate zero of an empty list. This case only
 * means anything while it is the first one in main(). */
static void test_print_before_any_candidate(void) {
    char captured[4096];

    capture_print(captured, sizeof captured);

    expect(occurrences(captured, "[GPUINFO] begin\n") == 1,
           "an uninitialised record still opens exactly one [GPUINFO] block");
    expect(occurrences(captured, "[GPUINFO] end\n") == 1, "and closes exactly one");
    expect(line_count(captured) == 4, "the empty block is exactly four lines");
    expect(line_is(captured, 0, "[GPUINFO] begin"), "line 0 opens the block");
    expect(line_is(captured, 1, "[GPUINFO] selected=none backend=\"none\" adapter=\"none\""),
           "nothing selected reads as none, never as index 0");
    expect(line_is(captured, 2, "[GPUINFO] candidates=0 dropped=0"),
           "the empty record says so rather than omitting the count");
    expect(line_is(captured, 3, "[GPUINFO] end"), "line 3 closes the block");
    expect(occurrences(captured, "[GPUINFO]") == line_count(captured),
           "every line carries the marker, so the block survives interleaving");
    expect(mdkr_gpu_info_get() != NULL && mdkr_gpu_info_get()->selected == -1,
           "the unread record reports selected == -1");
    expect(mdkr_gpu_info_get()->count == 0 && mdkr_gpu_info_get()->dropped == 0,
           "and holds no candidates");
}

/* 2. The shape with candidates in it, asserted whole. The free-text fields are
 * quoted so a parser can take them without guessing where they end. */
static void test_block_shape_with_candidates(void) {
    char                captured[4096];
    const MdkrGpuInfo  *info;

    mdkr_gpu_info_reset();
    note("webgpu-d3d12", "Intel(R) UHD Graphics 630", "31.0.101.2111", 0x8086u,
         0x3e9bu, "enumerated");
    note("webgpu-vulkan", "NVIDIA GeForce RTX 3070", "550.54.14", 0x10deu, 0x2484u,
         "enumerated");
    mdkr_gpu_info_select(1, "auto chose the discrete adapter");

    capture_print(captured, sizeof captured);

    expect(line_count(captured) == 6, "two candidates make a six-line block");
    expect(line_is(captured, 0, "[GPUINFO] begin"), "the block opens");
    expect(line_is(captured, 1,
                   "[GPUINFO] selected=1 backend=\"webgpu-vulkan\" "
                   "adapter=\"NVIDIA GeForce RTX 3070\""),
           "the summary line names the selected backend and adapter");
    expect(line_is(captured, 2, "[GPUINFO] candidates=2 dropped=0"),
           "the counts line follows the summary");
    expect(line_is(captured, 3,
                   "[GPUINFO] candidate=0 accepted=0 backend=\"webgpu-d3d12\" "
                   "adapter=\"Intel(R) UHD Graphics 630\" vendor=0x8086 device=0x3e9b "
                   "driver=\"31.0.101.2111\" reason=\"enumerated\""),
           "a rejected candidate prints every field it was given");
    expect(line_is(captured, 4,
                   "[GPUINFO] candidate=1 accepted=1 backend=\"webgpu-vulkan\" "
                   "adapter=\"NVIDIA GeForce RTX 3070\" vendor=0x10de device=0x2484 "
                   "driver=\"550.54.14\" reason=\"auto chose the discrete adapter\""),
           "the accepted candidate carries the selection reason");
    expect(line_is(captured, 5, "[GPUINFO] end"), "the block closes");

    info = mdkr_gpu_info_get();
    expect(info->count == 2 && info->selected == 1, "the record agrees with the block");

    /* Printing is a read. A second call must produce the same block, not a
     * longer one: the gate asserts exactly one block per run. */
    capture_print(captured, sizeof captured);
    expect(occurrences(captured, "[GPUINFO] begin\n") == 1 && line_count(captured) == 6,
           "printing twice prints the same block twice, and accumulates nothing");
}

/* 3. Missing fields. An adapter with no stated reason is the exact failure this
 * module exists to prevent, so an empty reason becomes an explicit placeholder
 * rather than an empty pair of quotes a reader would have to interpret. */
static void test_no_field_is_ever_reported_as_blank(void) {
    char               captured[4096];
    const MdkrGpuInfo *info;
    int                index;
    int                all_reasons_present = 1;

    mdkr_gpu_info_reset();
    note("", "", "", 0u, 0u, "");
    note("gl", "llvmpipe", "", 0x0u, 0x0u, "enumerated");

    info = mdkr_gpu_info_get();
    for (index = 0; index < info->count; index++) {
        if (info->candidates[index].reason[0] == '\0') {
            all_reasons_present = 0;
        }
    }
    expect(all_reasons_present, "no candidate is ever recorded with an empty reason");
    expect(strcmp(info->candidates[0].reason, "no reason recorded") == 0,
           "a candidate noted with no reason gets the placeholder, not silence");

    capture_print(captured, sizeof captured);
    expect(line_is(captured, 3,
                   "[GPUINFO] candidate=0 accepted=0 backend=\"unknown\" "
                   "adapter=\"unknown\" vendor=0x0000 device=0x0000 driver=\"unknown\" "
                   "reason=\"no reason recorded\""),
           "an entirely empty candidate prints as unknown, never as empty quotes");
    expect(line_is(captured, 4,
                   "[GPUINFO] candidate=1 accepted=0 backend=\"gl\" "
                   "adapter=\"llvmpipe\" vendor=0x0000 device=0x0000 "
                   "driver=\"unknown\" reason=\"enumerated\""),
           "a missing driver string reads as unknown next to the fields that are known");
    expect(occurrences(captured, "=\"\"") == 0, "no field is printed as an empty string");
}

/* 4. More adapters than the record holds. Eight is generous for a real machine
 * and the cost of being wrong is an overflow, so the surplus is counted and
 * reported instead of written. The first eight are kept because enumeration
 * order is the order the policy sees. */
static void test_overflow_is_counted_not_written(void) {
    char               captured[4096];
    const MdkrGpuInfo *info;
    char               name[64];
    int                index;

    mdkr_gpu_info_reset();
    for (index = 0; index < MDKR_GPU_MAX_CANDIDATES + 4; index++) {
        (void)snprintf(name, sizeof name, "adapter %d", index);
        note("gl", name, "1.0", 0x1002u, (uint32_t)index, "enumerated");
    }

    info = mdkr_gpu_info_get();
    expect(info->count == MDKR_GPU_MAX_CANDIDATES,
           "the record fills to capacity and stops");
    expect(info->dropped == 4, "and counts every candidate it had no room for");
    expect(strcmp(info->candidates[0].adapter, "adapter 0") == 0,
           "the first enumerated adapter is kept");
    expect(strcmp(info->candidates[MDKR_GPU_MAX_CANDIDATES - 1].adapter, "adapter 7") == 0,
           "and the last one that fit");

    capture_print(captured, sizeof captured);
    expect(line_is(captured, 2, "[GPUINFO] candidates=8 dropped=4"),
           "the block reports the drop, so a truncated list is never read as complete");
    expect(line_count(captured) == 4 + MDKR_GPU_MAX_CANDIDATES,
           "one line per kept candidate, plus the three frame lines and the summary");
    expect(occurrences(captured, "adapter 8") == 0, "a dropped candidate prints nothing");
}

/* 5. An index nobody enumerated. Refusing is the whole point: a select() that
 * trusted its argument would write past the array on a backend that miscounted,
 * and the record would then describe an adapter that does not exist. */
static void test_out_of_range_select_is_refused(void) {
    char               captured[4096];
    const MdkrGpuInfo *info;

    mdkr_gpu_info_reset();
    note("gl", "Adapter A", "1.0", 0x1002u, 0x1u, "enumerated");
    note("gl", "Adapter B", "1.0", 0x1002u, 0x2u, "enumerated");

    mdkr_gpu_info_select(2, "one past the end");
    info = mdkr_gpu_info_get();
    expect(info->selected == -1, "an index one past the end leaves selected at -1");

    mdkr_gpu_info_select(MDKR_GPU_MAX_CANDIDATES, "at capacity");
    expect(mdkr_gpu_info_get()->selected == -1, "so does an index at the array bound");

    mdkr_gpu_info_select(-1, "negative");
    expect(mdkr_gpu_info_get()->selected == -1, "so does a negative index");

    mdkr_gpu_info_select(2147483647, "absurd");
    expect(mdkr_gpu_info_get()->selected == -1, "so does an absurd one");

    info = mdkr_gpu_info_get();
    expect(info->count == 2, "a refused select does not change the candidate count");
    expect(strcmp(info->candidates[0].reason, "enumerated") == 0 &&
               strcmp(info->candidates[1].reason, "enumerated") == 0,
           "and writes its reason nowhere, not even into the nearest candidate");
    expect(info->candidates[0].accepted == 0 && info->candidates[1].accepted == 0,
           "nothing is marked accepted by a refused select");

    capture_print(captured, sizeof captured);
    expect(line_is(captured, 1, "[GPUINFO] selected=none backend=\"none\" adapter=\"none\""),
           "the block reports nothing selected rather than an adapter nobody enumerated");

    /* And the same record still accepts a real index afterwards. */
    mdkr_gpu_info_select(1, "picked after the refusals");
    expect(mdkr_gpu_info_get()->selected == 1,
           "a refused select does not poison the record for a valid one");
}

/* 6. Selection is the only thing that moves the accepted flag, and it carries
 * the reason the chooser gave. Exactly one candidate can be accepted, because a
 * block naming two winners tells the reader nothing. */
static void test_select_marks_exactly_one_candidate(void) {
    const MdkrGpuInfo *info;
    int                index;
    int                accepted_count = 0;

    mdkr_gpu_info_reset();
    note("gl", "Adapter A", "1.0", 0x1002u, 0x1u, "enumerated");
    note("webgpu-vulkan", "Adapter B", "1.0", 0x10deu, 0x2u, "enumerated");
    note("webgpu-d3d12", "Adapter C", "1.0", 0x8086u, 0x3u, "enumerated");

    mdkr_gpu_info_select(2, "high-performance chose the discrete adapter");
    info = mdkr_gpu_info_get();
    for (index = 0; index < info->count; index++) {
        accepted_count += info->candidates[index].accepted;
    }
    expect(info->selected == 2, "the chosen index is recorded");
    expect(accepted_count == 1 && info->candidates[2].accepted == 1,
           "exactly one candidate is accepted, and it is the chosen one");
    expect(strcmp(info->candidates[2].reason,
                  "high-performance chose the discrete adapter") == 0,
           "the selection reason replaces the enumeration reason");

    /* A second select moves the flag rather than adding one. A backend that
     * retries after a device request fails does exactly this. */
    mdkr_gpu_info_select(0, "vulkan device request failed, fell back to gl");
    info = mdkr_gpu_info_get();
    accepted_count = 0;
    for (index = 0; index < info->count; index++) {
        accepted_count += info->candidates[index].accepted;
    }
    expect(info->selected == 0 && accepted_count == 1,
           "re-selecting moves the accepted flag instead of adding a second one");
    expect(info->candidates[2].accepted == 0,
           "the previous winner is no longer marked accepted");
    expect(strcmp(info->candidates[2].reason,
                  "high-performance chose the discrete adapter") == 0,
           "but its recorded reason survives, so the reader sees the whole story");

    /* A chooser with nothing to say must not blank the reason it already had. */
    mdkr_gpu_info_select(1, "");
    info = mdkr_gpu_info_get();
    expect(strcmp(info->candidates[1].reason, "enumerated") == 0,
           "selecting with an empty reason keeps the reason already recorded");
    mdkr_gpu_info_select(1, NULL);
    expect(strcmp(mdkr_gpu_info_get()->candidates[1].reason, "enumerated") == 0,
           "and a NULL reason is the same refusal, not a crash");

    mdkr_gpu_info_reset();
    note("gl", "Adapter A", "1.0", 0x1002u, 0x1u, "");
    mdkr_gpu_info_select(0, "");
    expect(strcmp(mdkr_gpu_info_get()->candidates[0].reason, "no reason recorded") == 0,
           "with no reason anywhere the placeholder still stands in");
}

/* 7. What a backend actually hands over. Driver strings arrive from vendor
 * runtimes, so the record has to survive a field that is not terminated, and
 * text that would otherwise split one candidate across two lines. */
static void test_hostile_strings_cannot_break_the_block(void) {
    char               captured[4096];
    MdkrGpuCandidate   candidate;
    const MdkrGpuInfo *info;

    mdkr_gpu_info_reset();

    /* Not a string: every byte of both arrays is filled, with no terminator.
     * A copy that trusted the terminator reads off the end of the struct here,
     * which is what the sanitizers are for. */
    memset(&candidate, 0, sizeof candidate);
    memset(candidate.adapter, 'a', sizeof candidate.adapter);
    memset(candidate.backend, 'b', sizeof candidate.backend);
    (void)snprintf(candidate.driver, sizeof candidate.driver, "1.0");
    (void)snprintf(candidate.reason, sizeof candidate.reason, "enumerated");
    mdkr_gpu_info_note_candidate(&candidate);

    info = mdkr_gpu_info_get();
    expect(info->count == 1, "an unterminated field is still a candidate");
    expect(strlen(info->candidates[0].adapter) == MDKR_GPU_STR_MAX - 1u,
           "the copy stops one short of the field and terminates");
    expect(strlen(info->candidates[0].backend) == sizeof candidate.backend - 1u,
           "the short backend field is bounded by its own size, not the adapter's");

    /* One candidate is one line. A newline in a driver string would otherwise
     * forge a second candidate line, and a quote would end a quoted field
     * early — both would be parsed as truth by anything reading the block. */
    mdkr_gpu_info_reset();
    note("gl", "Fancy \"GPU\"", "2.0\nfake=1", 0x1002u, 0x5u, "why\tnot");
    capture_print(captured, sizeof captured);
    expect(line_count(captured) == 5, "hostile text still produces one line per candidate");
    expect(line_is(captured, 3,
                   "[GPUINFO] candidate=0 accepted=0 backend=\"gl\" "
                   "adapter=\"Fancy 'GPU'\" vendor=0x1002 device=0x0005 "
                   "driver=\"2.0 fake=1\" reason=\"why not\""),
           "control characters become spaces and quotes become apostrophes");
    expect(occurrences(captured, "\n[GPUINFO] end\n") == 1, "the block still ends once");
}

/* 8. Reset is what a backend retry calls, and a null candidate is what a
 * backend hands over when its own enumeration failed. Neither may leave debris
 * behind in a record that a player is about to paste into a bug report. */
static void test_reset_clears_and_null_is_ignored(void) {
    char               captured[4096];
    const MdkrGpuInfo *info;

    mdkr_gpu_info_reset();
    note("gl", "Adapter A", "1.0", 0x1002u, 0x1u, "enumerated");
    mdkr_gpu_info_select(0, "only candidate");
    mdkr_gpu_info_reset();

    info = mdkr_gpu_info_get();
    expect(info->count == 0 && info->dropped == 0 && info->selected == -1,
           "reset clears the count, the drops and the selection");

    mdkr_gpu_info_note_candidate(NULL);
    expect(mdkr_gpu_info_get()->count == 0, "a null candidate is ignored, not counted");

    capture_print(captured, sizeof captured);
    expect(line_count(captured) == 4 &&
               line_is(captured, 2, "[GPUINFO] candidates=0 dropped=0"),
           "and the block after a reset is the empty block again");
}

int main(void) {
    /* First, and only meaningful first: it is the one chance to observe a
     * record that nothing has initialised. */
    test_print_before_any_candidate();
    test_block_shape_with_candidates();
    test_no_field_is_ever_reported_as_blank();
    test_overflow_is_counted_not_written();
    test_out_of_range_select_is_refused();
    test_select_marks_exactly_one_candidate();
    test_hostile_strings_cannot_break_the_block();
    test_reset_clears_and_null_is_ignored();
    if (s_failures != 0) {
        printf("FAILURES: %d\n", s_failures);
        return 1;
    }
    printf("all gpu diagnostics assertions passed\n");
    return 0;
}
