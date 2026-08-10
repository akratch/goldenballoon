#include "a11y_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct A11ySlot {
    char             text[MDKR_A11Y_TEXT_MAX];
    MdkrA11yCategory category;
    MdkrA11yPriority priority;
    /* Superseded entries are tombstoned instead of compacted out of the ring:
     * the pop skips them, and the head/count arithmetic stays the obvious kind
     * that the Task 3 drain worker can reason about. */
    bool             live;
} A11ySlot;

static A11ySlot s_queue[MDKR_A11Y_QUEUE_MAX];
static size_t   s_head;  /* oldest occupied slot */
static size_t   s_count; /* occupied slots, live or tombstoned */
static bool     s_enabled[MDKR_A11Y_CAT_COUNT];
static bool     s_trace;
static bool     s_ready;

/* The utterance a consumer has taken but not yet finished speaking. */
static bool             s_in_flight;
static MdkrA11yPriority s_in_flight_priority;
static bool             s_in_flight_cancelled;

static const char *const k_category_names[MDKR_A11Y_CAT_COUNT] = {
    "focus", "section", "status", "race_position", "race_lap", "race_event"
};

static const char *const k_priority_names[] = { "low", "normal", "critical" };

void mdkr_a11y_init(void) {
    const char *trace = getenv("MDKR_A11Y_TRACE");
    int         index;

    memset(s_queue, 0, sizeof s_queue);
    s_head = 0u;
    s_count = 0u;
    for (index = 0; index < (int)MDKR_A11Y_CAT_COUNT; index++) {
        s_enabled[index] = true;
    }
    s_in_flight = false;
    s_in_flight_priority = MDKR_A11Y_PRI_NORMAL;
    s_in_flight_cancelled = false;
    s_trace = trace != NULL && trace[0] != '\0' && strcmp(trace, "0") != 0;
    s_ready = true;
}

/* The shell can announce before anything has called init(). Speaking is not
 * worth a crash or a silent no-op, so the first use configures the model. */
static void ensure_ready(void) {
    if (!s_ready) {
        mdkr_a11y_init();
    }
}

static bool category_valid(MdkrA11yCategory category) {
    const int index = (int)category;
    return index >= 0 && index < (int)MDKR_A11Y_CAT_COUNT;
}

static bool priority_valid(MdkrA11yPriority priority) {
    const int level = (int)priority;
    return level >= (int)MDKR_A11Y_PRI_LOW && level <= (int)MDKR_A11Y_PRI_CRITICAL;
}

static void trace_emit(MdkrA11yCategory category, MdkrA11yPriority priority,
                       const char *text) {
    if (!s_trace) {
        return;
    }
    /* One accepted utterance, one line, flushed: every accessibility gate reads
     * these interleaved with the rest of the run's output. */
    printf("[SPEAK] cat=%s pri=%s text=%s\n", k_category_names[category],
           k_priority_names[priority], text);
    fflush(stdout);
}

/* Kill the pending non-critical utterance in this category, if any. At most one
 * can exist, because this runs on every non-critical push. */
static void supersede_pending(MdkrA11yCategory category) {
    size_t offset;
    for (offset = 0u; offset < s_count; offset++) {
        A11ySlot *slot = &s_queue[(s_head + offset) % MDKR_A11Y_QUEUE_MAX];
        if (slot->live && slot->category == category &&
            slot->priority != MDKR_A11Y_PRI_CRITICAL) {
            slot->live = false;
        }
    }
}

static void queue_push(MdkrA11yCategory category, MdkrA11yPriority priority,
                       const char *text, size_t length) {
    size_t index;

    if (s_count == (size_t)MDKR_A11Y_QUEUE_MAX) {
        /* Full: drop the oldest. A player who tabbed through twenty settings
         * wants the twentieth, not the first. */
        s_head = (s_head + 1u) % MDKR_A11Y_QUEUE_MAX;
        s_count--;
    }
    index = (s_head + s_count) % MDKR_A11Y_QUEUE_MAX;
    memcpy(s_queue[index].text, text, length + 1u);
    s_queue[index].category = category;
    s_queue[index].priority = priority;
    s_queue[index].live = true;
    s_count++;
}

void mdkr_a11y_announce(MdkrA11yCategory category, MdkrA11yPriority priority,
                        const char *text) {
    size_t length;

    ensure_ready();
    if (!category_valid(category) || !priority_valid(priority)) {
        return;
    }
    if (!s_enabled[category]) {
        return; /* a disabled category is silent, in the trace as well */
    }
    if (text == NULL) {
        return;
    }
    length = strlen(text);
    if (length == 0u || length >= (size_t)MDKR_A11Y_TEXT_MAX) {
        return; /* refused whole: never truncate a sentence mid-word */
    }
    if (strpbrk(text, "\r\n") != NULL) {
        return; /* one utterance is one [SPEAK] line */
    }
    /* Two different rules, deliberately. Coalescing is per category: a focus
     * change must not delete a pending status line. Interruption is per
     * priority: anything newer barges in on what is being spoken, because the
     * player has moved on — unless that utterance is CRITICAL, which is the
     * whole reason the flag exists. */
    if (s_in_flight && s_in_flight_priority != MDKR_A11Y_PRI_CRITICAL) {
        s_in_flight_cancelled = true;
    }
    if (priority != MDKR_A11Y_PRI_CRITICAL) {
        supersede_pending(category);
    }
    queue_push(category, priority, text, length);
    trace_emit(category, priority, text);
}

/* Append one part of a composed utterance, or report that the whole thing has
 * to be refused. Never partially writes past what it reports. */
static bool compose_append(char *buffer, size_t capacity, size_t *length,
                           const char *part) {
    const size_t part_length = strlen(part);
    if (*length + part_length + 1u > capacity) {
        return false;
    }
    memcpy(buffer + *length, part, part_length + 1u);
    *length += part_length;
    return true;
}

void mdkr_a11y_focus(const char *name, const char *value, const char *help) {
    char   composed[MDKR_A11Y_TEXT_MAX];
    size_t length = 0u;

    ensure_ready();
    if (name == NULL || name[0] == '\0') {
        return; /* an unnamed control has nothing to say */
    }
    composed[0] = '\0';
    if (!compose_append(composed, sizeof composed, &length, name)) {
        return;
    }
    /* An absent part contributes nothing at all — printing it would put the
     * literal "(null)" into a player's ear. */
    if (value != NULL && value[0] != '\0') {
        if (!compose_append(composed, sizeof composed, &length, ", ") ||
            !compose_append(composed, sizeof composed, &length, value)) {
            return;
        }
    }
    if (help != NULL && help[0] != '\0') {
        if (!compose_append(composed, sizeof composed, &length, ". ") ||
            !compose_append(composed, sizeof composed, &length, help)) {
            return;
        }
    }
    mdkr_a11y_announce(MDKR_A11Y_CAT_FOCUS, MDKR_A11Y_PRI_NORMAL, composed);
}

int mdkr_a11y_next(char *out, size_t out_size, MdkrA11yCategory *out_category) {
    ensure_ready();
    if (out == NULL || out_size == 0u) {
        return 0;
    }
    for (;;) {
        A11ySlot *slot;
        size_t    length;

        if (s_count == 0u) {
            return 0; /* empty: `out` is left exactly as the caller had it */
        }
        slot = &s_queue[s_head];
        if (!slot->live) {
            s_head = (s_head + 1u) % MDKR_A11Y_QUEUE_MAX;
            s_count--;
            continue;
        }
        length = strlen(slot->text);
        if (length + 1u > out_size) {
            return 0; /* the caller cannot hold it; leave it queued, unclipped */
        }
        memcpy(out, slot->text, length + 1u);
        if (out_category != NULL) {
            *out_category = slot->category;
        }
        s_in_flight = true;
        s_in_flight_priority = slot->priority;
        s_in_flight_cancelled = false;
        s_head = (s_head + 1u) % MDKR_A11Y_QUEUE_MAX;
        s_count--;
        return 1;
    }
}

void mdkr_a11y_finish(void) {
    ensure_ready();
    s_in_flight = false;
    s_in_flight_cancelled = false;
}

bool mdkr_a11y_in_flight_cancelled(void) {
    ensure_ready();
    return s_in_flight && s_in_flight_cancelled;
}

void mdkr_a11y_set_category_enabled(MdkrA11yCategory category, bool enabled) {
    ensure_ready();
    if (!category_valid(category)) {
        return;
    }
    s_enabled[category] = enabled;
}

bool mdkr_a11y_category_enabled(MdkrA11yCategory category) {
    ensure_ready();
    return category_valid(category) && s_enabled[category];
}

void mdkr_a11y_set_trace(bool on) {
    ensure_ready();
    s_trace = on;
}
