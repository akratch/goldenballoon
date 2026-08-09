/* test_a11y_model.c — the accessibility model, asserted purely on its text.
 *
 * No audio device, no window, no speech engine: the whole point of the model is
 * that what a player would hear is decidable from the utterance stream and the
 * [SPEAK] trace, so every later accessibility gate can assert the same thing in
 * CI. These cases are that contract.
 */
#include "a11y_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
static int  dup_fd(int fd) { return _dup(fd); }
static int  replace_fd(int source, int target) { return _dup2(source, target); }
static void close_fd(int fd) { if (fd >= 0) { _close(fd); } }
static int  set_env(const char *name, const char *value) { return _putenv_s(name, value); }
static int  clear_env(const char *name) { return _putenv_s(name, ""); }
#else
#include <unistd.h>
static int  dup_fd(int fd) { return dup(fd); }
static int  replace_fd(int source, int target) { return dup2(source, target); }
static void close_fd(int fd) { if (fd >= 0) { (void)close(fd); } }
static int  set_env(const char *name, const char *value) { return setenv(name, value, 1); }
static int  clear_env(const char *name) { return unsetenv(name); }
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

/* Every case starts from the same state, with the trace forced off, so a
 * developer running with MDKR_A11Y_TRACE=1 exported sees the same result the
 * gate does. The trace case turns it on deliberately. */
static void reset(void) {
    mdkr_a11y_init();
    mdkr_a11y_set_trace(false);
}

static int pop(char *text, size_t size, MdkrA11yCategory *category) {
    return mdkr_a11y_next(text, size, category);
}

static int queued_count(void) {
    char text[MDKR_A11Y_TEXT_MAX];
    int  count = 0;
    while (pop(text, sizeof text, NULL) == 1) {
        count++;
    }
    return count;
}

/* 1. Coalescing: a player tabbing quickly must not hear a backlog. */
static void test_coalescing_keeps_only_the_newer(void) {
    char             text[MDKR_A11Y_TEXT_MAX];
    MdkrA11yCategory category = MDKR_A11Y_CAT_RACE_EVENT;

    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Brightness, 50");
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Contrast, 60");
    expect(pop(text, sizeof text, &category) == 1, "a coalesced pair still speaks");
    expect(strcmp(text, "Contrast, 60") == 0,
           "two focus announcements in a row yield only the second");
    expect(category == MDKR_A11Y_CAT_FOCUS, "the category travels with the utterance");
    expect(pop(text, sizeof text, NULL) == 0, "the superseded announcement is gone");

    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_LOW, "Brightness, 50");
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Contrast, 60");
    expect(queued_count() == 1, "LOW and NORMAL coalesce with each other");

    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Brightness, 50");
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, "Settings applied");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Brightness, 50") == 0,
           "a different category does not coalesce, and order is oldest first");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Settings applied") == 0,
           "the second category follows in order");

    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_CRITICAL, "Final lap");
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_NORMAL, "Banana collected");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Final lap") == 0,
           "a queued CRITICAL is not coalesced away by a later NORMAL");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Banana collected") == 0,
           "and the NORMAL still follows it");
}

/* 2. A CRITICAL utterance already in flight is not cancelled by a focus change.
 * "In flight" = popped by mdkr_a11y_next() and not yet mdkr_a11y_finish()ed.
 * Interruption is decided by priority alone, so each case below has to differ
 * from its control in priority, not in category — otherwise the case would pass
 * with the CRITICAL rule deleted. */
static void test_critical_in_flight_is_not_cancelled(void) {
    char text[MDKR_A11Y_TEXT_MAX];
    char over_long[MDKR_A11Y_TEXT_MAX + 8];

    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_CRITICAL, "Final lap");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Final lap") == 0,
           "the critical utterance is handed to the voice");
    mdkr_a11y_focus("Music volume", "80", "Sets how loud the music is.");
    expect(!mdkr_a11y_in_flight_cancelled(),
           "a focus change does not cancel a CRITICAL utterance in flight");
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_NORMAL, "Banana collected");
    expect(!mdkr_a11y_in_flight_cancelled(),
           "nor does a newer announcement in the critical utterance's own category");
    mdkr_a11y_finish();
    expect(!mdkr_a11y_in_flight_cancelled(), "nothing is in flight once it is finished");
    expect(pop(text, sizeof text, NULL) == 1 && strstr(text, "Music volume") != NULL,
           "the focus utterance is still delivered, after the critical one");

    /* Controls for the two cases above: same categories, priority lowered. If
     * the CRITICAL exemption were removed, these would still pass and the two
     * assertions above would fail — which is what makes them mean something. */
    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_NORMAL, "Final lap");
    expect(pop(text, sizeof text, NULL) == 1, "the same utterance, only NORMAL, goes in flight");
    mdkr_a11y_focus("Music volume", "80", "Sets how loud the music is.");
    expect(mdkr_a11y_in_flight_cancelled(),
           "a focus change does cancel a NORMAL utterance in flight");

    reset();
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Brightness, 50");
    expect(pop(text, sizeof text, NULL) == 1, "the focus utterance goes in flight");
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Contrast, 60");
    expect(mdkr_a11y_in_flight_cancelled(),
           "a newer utterance in the same category cancels a non-critical one in flight");
    mdkr_a11y_finish();
    expect(!mdkr_a11y_in_flight_cancelled(), "finishing clears the cancellation");

    /* An announcement that is refused produces no utterance at all, so it must
     * not silently stop the voice either. */
    reset();
    memset(over_long, 'a', sizeof over_long);
    over_long[MDKR_A11Y_TEXT_MAX] = '\0';
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Brightness, 50");
    expect(pop(text, sizeof text, NULL) == 1, "the focus utterance goes in flight again");
    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_STATUS, false);
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, "Settings applied");
    expect(!mdkr_a11y_in_flight_cancelled(),
           "a filtered-out announcement does not cancel what is being spoken");
    mdkr_a11y_announce(MDKR_A11Y_CAT_SECTION, MDKR_A11Y_PRI_NORMAL, over_long);
    expect(!mdkr_a11y_in_flight_cancelled(),
           "a refused over-long announcement does not cancel what is being spoken");
    mdkr_a11y_announce(MDKR_A11Y_CAT_SECTION, MDKR_A11Y_PRI_NORMAL, "Audio settings");
    expect(mdkr_a11y_in_flight_cancelled(),
           "an accepted announcement in any category does cancel it");

    reset();
    expect(!mdkr_a11y_in_flight_cancelled(), "nothing in flight cancels nothing");
}

/* 3. A disabled category produces no utterance at all. */
static void test_disabled_category_is_silent(void) {
    char text[MDKR_A11Y_TEXT_MAX];

    reset();
    expect(mdkr_a11y_category_enabled(MDKR_A11Y_CAT_RACE_POSITION),
           "categories start enabled");
    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_RACE_POSITION, false);
    expect(!mdkr_a11y_category_enabled(MDKR_A11Y_CAT_RACE_POSITION),
           "the category reads back disabled");
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_POSITION, MDKR_A11Y_PRI_CRITICAL, "Second place");
    expect(pop(text, sizeof text, NULL) == 0,
           "a disabled category produces no utterance, not even a CRITICAL one");

    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_LAP, MDKR_A11Y_PRI_NORMAL, "Lap 2 of 3");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Lap 2 of 3") == 0,
           "disabling one category leaves the others speaking");

    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_RACE_POSITION, true);
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_POSITION, MDKR_A11Y_PRI_NORMAL, "First place");
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "First place") == 0,
           "re-enabling a category restores it");

    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_FOCUS, false);
    mdkr_a11y_focus("Brightness", "50", "How bright the picture is.");
    expect(pop(text, sizeof text, NULL) == 0,
           "mdkr_a11y_focus() honours the focus category filter too");
    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_FOCUS, true);

    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_COUNT, false);
    expect(!mdkr_a11y_category_enabled(MDKR_A11Y_CAT_COUNT),
           "a category outside the range is never enabled");
    mdkr_a11y_announce(MDKR_A11Y_CAT_COUNT, MDKR_A11Y_PRI_NORMAL, "nowhere");
    expect(pop(text, sizeof text, NULL) == 0, "an out-of-range category is dropped");
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, "Settings applied");
    expect(pop(text, sizeof text, NULL) == 1,
           "and it did not disable a real category on the way past");
}

/* 4. The queue is bounded and drops the oldest, never the newest. */
static void test_queue_is_bounded_and_keeps_the_newest(void) {
    char text[MDKR_A11Y_TEXT_MAX];
    char first[MDKR_A11Y_TEXT_MAX];
    char expected_first[MDKR_A11Y_TEXT_MAX];
    char last[MDKR_A11Y_TEXT_MAX];
    int  index;
    int  count = 0;

    reset();
    first[0] = '\0';
    last[0] = '\0';
    for (index = 0; index < 1000; index++) {
        /* CRITICAL so that coalescing cannot be what bounds the queue: this
         * case has to prove the ring's capacity, not the supersede rule. */
        (void)snprintf(text, sizeof text, "utterance %d", index);
        mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_CRITICAL, text);
    }
    while (pop(text, sizeof text, NULL) == 1) {
        if (count == 0) {
            (void)snprintf(first, sizeof first, "%s", text);
        }
        (void)snprintf(last, sizeof last, "%s", text);
        count++;
    }
    expect(count == MDKR_A11Y_QUEUE_MAX,
           "1,000 announcements leave exactly the ring's fixed capacity");
    expect(count < 1000, "the queue is bounded");
    (void)snprintf(expected_first, sizeof expected_first, "utterance %d",
                   1000 - MDKR_A11Y_QUEUE_MAX);
    expect(strcmp(first, expected_first) == 0,
           "the oldest survivor is exactly capacity-many back from the end");
    expect(strcmp(last, "utterance 999") == 0,
           "the newest announcement survived; the oldest were dropped");
}

/* 5. Over-long text is refused whole. Mid-word truncation speaks nonsense. */
static void test_over_long_text_is_rejected_not_truncated(void) {
    char text[MDKR_A11Y_TEXT_MAX];
    char over_long[MDKR_A11Y_TEXT_MAX + 8];

    reset();
    memset(over_long, 'a', sizeof over_long);
    over_long[MDKR_A11Y_TEXT_MAX] = '\0'; /* one character over the limit */
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, over_long);
    expect(pop(text, sizeof text, NULL) == 0,
           "text longer than MDKR_A11Y_TEXT_MAX - 1 is refused, not truncated");

    over_long[MDKR_A11Y_TEXT_MAX - 1] = '\0'; /* exactly the limit */
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, over_long);
    expect(pop(text, sizeof text, NULL) == 1 &&
               strlen(text) == (size_t)MDKR_A11Y_TEXT_MAX - 1u,
           "the longest legal utterance is kept whole");

    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, "");
    expect(pop(text, sizeof text, NULL) == 0, "empty text says nothing");
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, NULL);
    expect(pop(text, sizeof text, NULL) == 0, "NULL text says nothing");
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, "two\nlines");
    expect(pop(text, sizeof text, NULL) == 0,
           "an embedded newline is refused: one utterance is one [SPEAK] line");

    /* A focus utterance whose parts do not fit is refused whole as well. */
    over_long[MDKR_A11Y_TEXT_MAX - 1] = '\0';
    mdkr_a11y_focus("Brightness", over_long, "How bright the picture is.");
    expect(pop(text, sizeof text, NULL) == 0,
           "a focus composition that would overflow is refused, not clipped");
}

/* 6. An empty pop returns 0 and does not touch the caller's buffer. */
static void test_empty_pop_leaves_out_untouched(void) {
    char             out[MDKR_A11Y_TEXT_MAX];
    MdkrA11yCategory category = MDKR_A11Y_CAT_RACE_LAP;
    int              intact = 1;
    size_t           index;

    reset();
    memset(out, '#', sizeof out); /* deliberately not NUL-terminated */
    expect(mdkr_a11y_next(out, sizeof out, &category) == 0,
           "mdkr_a11y_next() on an empty queue returns 0");
    for (index = 0; index < sizeof out; index++) {
        if (out[index] != '#') {
            intact = 0;
        }
    }
    expect(intact, "an empty pop writes nothing to out; the sentinel survives");
    expect(category == MDKR_A11Y_CAT_RACE_LAP,
           "an empty pop does not write the out category either");

    /* A buffer too small for the queued text is the same refusal: the caller
     * gets nothing rather than half a sentence, and the utterance stays put. */
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, "Settings applied");
    memset(out, '#', sizeof out);
    expect(mdkr_a11y_next(out, 4u, &category) == 0, "a too-small buffer pops nothing");
    expect(out[0] == '#', "and it is not written with a truncated utterance");
    expect(mdkr_a11y_next(out, sizeof out, &category) == 1 &&
               strcmp(out, "Settings applied") == 0,
           "the utterance is still queued for a caller that can hold it");
}

/* 7. mdkr_a11y_focus() composes exactly one utterance, in category FOCUS. */
static void test_focus_composes_one_utterance(void) {
    char             text[MDKR_A11Y_TEXT_MAX];
    MdkrA11yCategory category = MDKR_A11Y_CAT_RACE_LAP;

    reset();
    mdkr_a11y_focus("Music volume", "80 percent", "Sets how loud the music is.");
    expect(pop(text, sizeof text, &category) == 1, "focus produces an utterance");
    expect(category == MDKR_A11Y_CAT_FOCUS, "focus utterances are category FOCUS");
    expect(strstr(text, "Music volume") != NULL, "the composed text names the control");
    expect(strstr(text, "80 percent") != NULL, "the composed text carries the value");
    expect(strstr(text, "Sets how loud the music is.") != NULL,
           "the composed text carries the help");
    expect(pop(text, sizeof text, NULL) == 0, "focus composes exactly one utterance");

    reset();
    mdkr_a11y_focus("Fullscreen", NULL, "Fills the whole display.");
    expect(pop(text, sizeof text, &category) == 1, "a control with no value still speaks");
    expect(strstr(text, "(null)") == NULL, "a NULL value never reaches the voice as (null)");
    expect(strstr(text, "Fullscreen") != NULL && strstr(text, "Fills the whole display.") != NULL,
           "the remaining parts are both present");

    reset();
    mdkr_a11y_focus("Fullscreen", "on", NULL);
    expect(pop(text, sizeof text, NULL) == 1, "a control with no help still speaks");
    expect(strstr(text, "(null)") == NULL, "a NULL help never reaches the voice as (null)");
    expect(strstr(text, "Fullscreen") != NULL && strstr(text, "on") != NULL,
           "the name and value are both present");

    reset();
    mdkr_a11y_focus("Back", NULL, NULL);
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Back") == 0,
           "a bare name speaks as itself");

    reset();
    mdkr_a11y_focus(NULL, "80", "Sets how loud the music is.");
    expect(pop(text, sizeof text, NULL) == 0, "a control with no name says nothing");
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

/* 8. The [SPEAK] trace: exactly one line per accepted utterance, and silence
 * when the trace is off. Every later accessibility gate parses these lines. */
static void test_trace_emits_one_line_per_accepted_utterance(void) {
    static const char expected[] =
        "[SPEAK] cat=focus pri=normal text=Brightness, 50\n"
        "[SPEAK] cat=race_lap pri=critical text=Final lap\n"
        "[SPEAK] cat=focus pri=normal text=Music volume, 80. Sets how loud the music is.\n";
    char captured[4096];
    char over_long[MDKR_A11Y_TEXT_MAX + 8];
    char text[MDKR_A11Y_TEXT_MAX];

    memset(over_long, 'a', sizeof over_long);
    over_long[MDKR_A11Y_TEXT_MAX] = '\0';

    reset();
    if (!capture_begin()) {
        expect(0, "stdout can be captured for the trace case");
        return;
    }
    mdkr_a11y_set_trace(true);
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Brightness, 50");
    mdkr_a11y_announce(MDKR_A11Y_CAT_RACE_LAP, MDKR_A11Y_PRI_CRITICAL, "Final lap");
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL, over_long);
    mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_SECTION, false);
    mdkr_a11y_announce(MDKR_A11Y_CAT_SECTION, MDKR_A11Y_PRI_NORMAL, "Audio settings");
    mdkr_a11y_focus("Music volume", "80", "Sets how loud the music is.");
    capture_end(captured, sizeof captured);

    if (strcmp(captured, expected) != 0) {
        printf("captured trace was:\n%s", captured);
    }
    expect(strcmp(captured, expected) == 0,
           "trace on emits one exact [SPEAK] line per accepted utterance");
    /* The superseded focus line is still traced but not queued: the trace
     * records what the app said, the queue holds what will be spoken. */
    expect(pop(text, sizeof text, NULL) == 1 && strcmp(text, "Final lap") == 0,
           "the coalesced focus line did not add a second queued utterance");
    expect(pop(text, sizeof text, NULL) == 1 &&
               strcmp(text, "Music volume, 80. Sets how loud the music is.") == 0,
           "the surviving focus utterance is the newest one");
    expect(pop(text, sizeof text, NULL) == 0, "and nothing else was queued");

    if (!capture_begin()) {
        expect(0, "stdout can be captured for the trace-off case");
        return;
    }
    mdkr_a11y_set_trace(false);
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Contrast, 60");
    mdkr_a11y_focus("Fullscreen", "on", "Fills the whole display.");
    capture_end(captured, sizeof captured);
    expect(captured[0] == '\0', "trace off emits nothing at all");

    /* MDKR_A11Y_TRACE=1 is how the shell and race gates arm this, so the env
     * route is asserted rather than assumed. */
    expect(set_env("MDKR_A11Y_TRACE", "1") == 0, "the trace variable can be set");
    if (!capture_begin()) {
        expect(0, "stdout can be captured for the env case");
        return;
    }
    mdkr_a11y_init();
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Brightness, 50");
    capture_end(captured, sizeof captured);
    expect(strcmp(captured, "[SPEAK] cat=focus pri=normal text=Brightness, 50\n") == 0,
           "MDKR_A11Y_TRACE=1 arms the trace at init, with no set_trace() call");

    expect(set_env("MDKR_A11Y_TRACE", "0") == 0, "the trace variable can be cleared");
    if (!capture_begin()) {
        expect(0, "stdout can be captured for the env-off case");
        return;
    }
    mdkr_a11y_init();
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, "Contrast, 60");
    capture_end(captured, sizeof captured);
    expect(captured[0] == '\0', "MDKR_A11Y_TRACE=0 leaves the trace off");
    (void)clear_env("MDKR_A11Y_TRACE");
}

int main(void) {
    test_coalescing_keeps_only_the_newer();
    test_critical_in_flight_is_not_cancelled();
    test_disabled_category_is_silent();
    test_queue_is_bounded_and_keeps_the_newest();
    test_over_long_text_is_rejected_not_truncated();
    test_empty_pop_leaves_out_untouched();
    test_focus_composes_one_utterance();
    test_trace_emits_one_line_per_accepted_utterance();
    if (s_failures != 0) {
        printf("FAILURES: %d\n", s_failures);
        return 1;
    }
    printf("all accessibility model assertions passed\n");
    return 0;
}
