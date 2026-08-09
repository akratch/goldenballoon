#include "a11y_race.h"

#include "a11y_model.h"
#include "gameplay_event_trace.h"
#include "video_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The port-1 human racer's authoritative race state for one tick. Everything
 * here is a scalar race_check_finish() has already decided; this file only
 * compares consecutive copies and phrases the differences.
 */
typedef struct A11yRaceSnapshot {
    int position;
    int racerCount;
    int lap;        /* zero-based, as the racer stores it */
    int lapCount;
    int finished;
    int finishPosition;
    int itemQuantity;
    int itemType;
} A11yRaceSnapshot;

/* Published by the race probe, consumed at the tick boundary. The sequence
 * number is what distinguishes "the race published the same numbers again"
 * from "the race is no longer running", which no field of the snapshot can. */
static A11yRaceSnapshot s_published;
static uint32_t         s_publishSeq;
static uint32_t         s_consumedSeq;

static bool s_enabled;
static bool s_categoriesApplied;

static bool             s_inRace;
static A11yRaceSnapshot s_spoken;      /* what the player has been told */
static uint64_t         s_lastPositionTick;
static uint64_t         s_lastPublishTick;

/*
 * Ticks of silence from race_check_finish() after which the race is over as far
 * as this layer is concerned. Generous: a level transition or a results screen
 * stops the publication long before this, and the only cost of waiting is that
 * a paused race is not mistaken for a finished one.
 */
#define RACE_IDLE_TICKS 120u

static const char *const k_ordinals[] = {
    "first", "second", "third",   "fourth",
    "fifth", "sixth",  "seventh", "eighth"
};

/* BalloonType (game/include/enums.h) in its own words. A player who cannot see
 * the item icon needs the name, not the index. */
static const char *item_name(int itemType) {
    switch (itemType) {
        case 0:  return "boost";
        case 1:  return "missiles";
        case 2:  return "traps";
        case 3:  return "a shield";
        case 4:  return "a magnet";
        default: return "an item";
    }
}

static const char *ordinal(int place) {
    if (place >= 1 && place <= (int)(sizeof k_ordinals / sizeof k_ordinals[0])) {
        return k_ordinals[place - 1];
    }
    return NULL;
}

/* "third" -> "Third". Written out because an ordinal both opens a sentence and
 * appears inside one, and a voice reading "Race started. third of eight" is
 * reading a sentence nobody wrote. */
static void capitalize(char *text) {
    if (text[0] >= 'a' && text[0] <= 'z') {
        text[0] = (char)(text[0] - 'a' + 'A');
    }
}

static void announce(MdkrA11yCategory category, MdkrA11yPriority priority,
                     const char *text) {
    if (!s_enabled) {
        return;
    }
    mdkr_a11y_announce(category, priority, text);
}

/*
 * Race announcements are on when the player has speech on AND has not switched
 * the race calls off. Both live in the durable player settings, so a config
 * file, MDKR_A11Y_SPEECH / MDKR_A11Y_SPEECH_RACE, and the Accessibility panel
 * all reach this the same way. Read live, because Accessibility.Speech is a
 * LIVE key: switching it on mid-session has to start the calls.
 */
static bool read_enabled(void) {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    if (config == NULL) {
        return false;
    }
    return config->values[MDKR_A11Y_SPEECH].number != 0.0f &&
           config->values[MDKR_A11Y_SPEECH_RACE].number != 0.0f;
}

/*
 * Per-category enable, from MDKR_A11Y_RACE_CATEGORIES: a comma-separated list
 * of the categories to keep, out of "position", "lap" and "event". Unset means
 * all three, which is what a player who has never thought about it should get.
 */
static bool category_listed(const char *list, const char *name) {
    const size_t length = strlen(name);
    const char  *cursor = list;
    while (*cursor != '\0') {
        const char  *end = strchr(cursor, ',');
        const size_t span =
            (end != NULL) ? (size_t)(end - cursor) : strlen(cursor);
        if (span == length && strncmp(cursor, name, length) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

bool mdkr_a11y_race_set_category(const char *name, bool enabled) {
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "position") == 0) {
        mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_RACE_POSITION, enabled);
        return true;
    }
    if (strcmp(name, "lap") == 0) {
        mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_RACE_LAP, enabled);
        return true;
    }
    if (strcmp(name, "event") == 0) {
        mdkr_a11y_set_category_enabled(MDKR_A11Y_CAT_RACE_EVENT, enabled);
        return true;
    }
    return false;
}

static void apply_category_selection(void) {
    static const char *const k_names[] = { "position", "lap", "event" };
    const char *list;
    size_t      index;

    if (s_categoriesApplied) {
        return;
    }
    s_categoriesApplied = true;
    list = getenv("MDKR_A11Y_RACE_CATEGORIES");
    if (list == NULL) {
        return;
    }
    for (index = 0u; index < sizeof k_names / sizeof k_names[0]; index++) {
        (void)mdkr_a11y_race_set_category(
            k_names[index], category_listed(list, k_names[index]));
    }
}

/* A new level means a new race: forget everything said about the last one, so
 * the next race announces its start rather than continuing the previous one. */
static void reset_race(void) {
    s_inRace = false;
    memset(&s_spoken, 0, sizeof s_spoken);
    s_lastPositionTick = 0u;
}

/*
 * Deliberately does NOT state a position. Every racer's racePosition is 1 until
 * race_check_finish() has run its own three-tick debounce, so a grid-time
 * position is not merely early, it is wrong -- it would tell a player starting
 * eighth that they are first. The position call that follows a second or two
 * later is the first one that is true, and saying nothing until then is the
 * only honest option.
 */
static void speak_start(const A11yRaceSnapshot *now) {
    char text[MDKR_A11Y_TEXT_MAX];

    if (now->racerCount > 1) {
        snprintf(text, sizeof text, "Race started. %d racers, lap 1 of %d.",
                 now->racerCount, now->lapCount);
    } else {
        snprintf(text, sizeof text, "Race started. Lap 1 of %d.",
                 now->lapCount);
    }
    announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_NORMAL, text);
}

static void speak_position(const A11yRaceSnapshot *now) {
    char        text[MDKR_A11Y_TEXT_MAX];
    const char *word = ordinal(now->position);

    if (word == NULL) {
        return;
    }
    if (now->racerCount > 1) {
        snprintf(text, sizeof text, "%s of %d", word, now->racerCount);
    } else {
        snprintf(text, sizeof text, "%s", word);
    }
    capitalize(text);
    announce(MDKR_A11Y_CAT_RACE_POSITION, MDKR_A11Y_PRI_NORMAL, text);
}

static void speak_lap(const A11yRaceSnapshot *now) {
    char      text[MDKR_A11Y_TEXT_MAX];
    const int shown = now->lap + 1;

    if (shown > now->lapCount) {
        /* The racer's lap counter reaches the lap total on the tick it crosses
         * the line for the last time. That tick is the finish, not a fourth lap
         * of a three-lap race. */
        return;
    }
    snprintf(text, sizeof text, "Lap %d of %d", shown, now->lapCount);
    announce(MDKR_A11Y_CAT_RACE_LAP, MDKR_A11Y_PRI_NORMAL, text);
    if (shown == now->lapCount) {
        /* Its own line and its own category: the final lap is the one moment a
         * player changes how they drive, and it has to survive a player who has
         * switched the routine lap calls off. */
        announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_NORMAL, "Final lap");
    }
}

static void speak_finish(const A11yRaceSnapshot *now) {
    char        text[MDKR_A11Y_TEXT_MAX];
    const char *word = ordinal(now->finishPosition);

    if (word != NULL) {
        snprintf(text, sizeof text, "Finished %s", word);
    } else {
        snprintf(text, sizeof text, "Finished");
    }
    /* CRITICAL: the result is the one line that must not be cut off by whatever
     * happens next. */
    announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_CRITICAL, text);
}

static void speak_item(const A11yRaceSnapshot *now) {
    char text[MDKR_A11Y_TEXT_MAX];

    snprintf(text, sizeof text, "Picked up %s", item_name(now->itemType));
    announce(MDKR_A11Y_CAT_RACE_EVENT, MDKR_A11Y_PRI_NORMAL, text);
}

static void observe_tick(uint64_t tick) {
    A11yRaceSnapshot now;

    if (!s_enabled) {
        return;
    }
    if (s_publishSeq == s_consumedSeq) {
        /* race_check_finish() did not run this tick. Long enough of that and
         * the race is over: results, a menu, a level transition. */
        if (s_inRace && tick - s_lastPublishTick > (uint64_t)RACE_IDLE_TICKS) {
            reset_race();
        }
        return;
    }
    s_consumedSeq = s_publishSeq;
    s_lastPublishTick = tick;
    now = s_published;
    if (now.lapCount <= 0 || now.racerCount <= 0) {
        return; /* not a shape this layer can describe honestly */
    }

    if (!s_inRace) {
        s_inRace = true;
        s_spoken = now;
        /* Position is not yet known (see speak_start): leave it at a value no
         * racer can hold, so the first settled position is announced rather
         * than compared against a grid-time placeholder. */
        s_spoken.position = 0;
        s_lastPositionTick = tick;
        speak_start(&now);
        return;
    }

    if (now.lap > s_spoken.lap) {
        speak_lap(&now);
    }
    if (now.finished && !s_spoken.finished) {
        speak_finish(&now);
    }
    if (now.itemQuantity > s_spoken.itemQuantity) {
        speak_item(&now);
    }
    /*
     * The coalescing. Intermediate positions are deliberately never spoken:
     * what a player needs is where they are NOW, so the newest value is held
     * and said once the previous line has had time to finish. Comparing against
     * what was last SPOKEN rather than against the previous tick is what makes
     * a swap and a swap back cost nothing at all.
     */
    if (now.position != s_spoken.position && !now.finished &&
        tick - s_lastPositionTick >=
            (uint64_t)MDKR_A11Y_RACE_POSITION_MIN_TICKS) {
        speak_position(&now);
        s_lastPositionTick = tick;
        s_spoken.position = now.position;
    }
    s_spoken.lap = now.lap;
    s_spoken.finished = now.finished;
    s_spoken.finishPosition = now.finishPosition;
    s_spoken.itemQuantity = now.itemQuantity;
    s_spoken.itemType = now.itemType;
    s_spoken.racerCount = now.racerCount;
    s_spoken.lapCount = now.lapCount;
}

static void observe_event(GameplayEventKind kind, int32_t a, int32_t b,
                          int32_t c, int32_t d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    if (kind == GAMEPLAY_EVENT_LEVEL) {
        reset_race();
    }
}

static const GameplayEventObserver k_observer = { observe_event, observe_tick };

/* Arm or disarm in one place, so "speech is off" really does mean the event
 * sites are not armed and nothing is observing them. */
static void set_enabled(bool enabled) {
    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    if (enabled) {
        apply_category_selection();
        gameplay_event_trace_set_observer(&k_observer);
    } else {
        gameplay_event_trace_set_observer(NULL);
        reset_race();
    }
}

void mdkr_a11y_race_init(void) {
    set_enabled(read_enabled());
}

bool mdkr_a11y_race_enabled(void) {
    return s_enabled;
}

void mdkr_a11y_race_publish(int racePosition, int racerCount,
                            int lap, int lapCount,
                            int raceFinished, int finishPosition,
                            int itemQuantity, int itemType) {
    /* Once per authoritative tick, and only while a race is running, so
     * re-reading the live setting here is what makes the option take effect
     * without a restart at a cost nobody can measure. */
    set_enabled(read_enabled());
    if (!s_enabled) {
        return;
    }
    s_published.position = racePosition;
    s_published.racerCount = racerCount;
    s_published.lap = lap;
    s_published.lapCount = lapCount;
    s_published.finished = raceFinished;
    s_published.finishPosition = finishPosition;
    s_published.itemQuantity = itemQuantity;
    s_published.itemType = itemType;
    s_publishSeq++;
}
