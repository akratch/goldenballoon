/**
 * mdkr_adventure.c -- Adventure-mode TEST HOOKS (native port only).
 *
 * Every hook here is a no-op unless its environment variable is set -- the same
 * contract as MDKR_FORCE_BOOST / MDKR_FORCE_LAPS / MDKR_AUTOPILOT /
 * MDKR_LOAD_TRACK. The driving hook is called at the end of
 * update_player_racer()'s input dispatch; the result-verdict hook is called
 * after natural finish assignment in race_check_finish().
 *
 * ---------------------------------------------------------------------------
 * Why this exists
 * ---------------------------------------------------------------------------
 * Timber's Island (ASSET_LEVEL_CENTRALAREAHUB) was drivable but no headless run
 * had ever ENTERED a race from it.  Entering one means driving through a
 * BHV_EXIT object: obj_loop_exit() latches racer->exitObj, racer_enter_door()
 * then drives the last stretch and calls func_8006D968() on the exit's
 * level_entry.  Two properties make that unreachable by scripted input:
 *
 *   - it is a point target: the exit only latches inside its activation radius
 *     AND on the correct side of its facing plane
 *     (dot(dir, racer - exit) < 0, obj_loop_exit), and
 *   - MDKR_AUTOPILOT=1 follows the hub's AI-node spline, which stays in the
 *     central beach loop -- measured bounding box x -1633..1761, z -1699..2025
 *     over 6133 frames (cp 66, lap 6), whose closest approach to ANY hub exit is
 *     2144 units.  It laps the island forever without ever turning in at a door.
 *
 * 18 hand-timed open-loop stick routes had already failed (docs/OPEN_ITEMS.md
 * "P3.5 Adventure").  The missing capability is closed-loop steering toward a
 * known object, which is what this provides.
 *
 * ---------------------------------------------------------------------------
 * The hooks
 * ---------------------------------------------------------------------------
 *   MDKR_OBJDUMP=1   (=2 also dumps every AI node and its adjacency)
 *       Once per level, print every BHV_EXIT, BHV_DOOR/BHV_TT_DOOR,
 *       BHV_GOLDEN_BALLOON and dialogue NPC with its world position and the
 *       fields that decide whether the player may pass.  Everything is read from
 *       the live spawned object plus its on-disk level_entry, so a mis-decoded
 *       object map shows up here as nonsense rather than as an unexplained
 *       failure to enter.  This is how every object id in the route fixtures was
 *       found -- no offline object-map parser to disagree with the game.
 *
 *   MDKR_DRIVE_ROUTE="<levelId>:<step>[:<step>...][;<levelId>:...]"
 *       Steer the human racer through an ordered list of steps, per level.
 *       Levels not named are left completely alone, so MDKR_AUTOPILOT=1 can
 *       drive the race itself.  Steps:
 *
 *         E<destMapId>   the BHV_EXIT whose LevelObjectEntry_Exit
 *                        .destinationMapId is <destMapId>.  Names the level you
 *                        want to ENTER rather than a coordinate, and takes the
 *                        position from the object the game actually spawned.
 *                        Retires when the level actually changes while it is the
 *                        active step, which is the only observable "this exit was
 *                        used".
 *         B<balloonID>   the BHV_GOLDEN_BALLOON with that balloonID.  Completes
 *                        only when the balloon is really COLLECTED, i.e. when
 *                        courseFlagsPtr[courseId] gains bit (0x10000 <<
 *                        balloonID) -- the same flag obj_loop_goldenballoon
 *                        sets.  A proximity test would not do: collection needs
 *                        interactObj->distance < 45 (a 3D distance), so
 *                        "close enough to steer past" is not "collected".
 *         T              the BHV_TROPHY_CABINET in the current world lobby.
 *                        The step deliberately has no proximity completion:
 *                        only the cabinet's production collision/dialogue path
 *                        can enter the trophy series.
 *         <x>,<z>        a plain waypoint, completed within MDKR_DRIVE_WPR
 *                        units (default 220).
 *
 *       Steps complete in order, are never revisited, and PERSIST per level
 *       across leaving and re-entering it.  That is what lets one route both
 *       enter a race from the world lobby and, on the way back out, leave the
 *       lobby for Timber's Island -- and it means the route cannot oscillate
 *       between two steps.
 *
 *   MDKR_DRIVE_WPR=<units>       waypoint completion radius (default 220)
 *   MDKR_DRIVE_VERBOSE=1         per-30-frame progress trace (needs MDKR_TRACE)
 *
 *   MDKR_BOSS_ROUTE=1
 *       Preserve the stock Tricky 1 AI line through checkpoint 35, then steer
 *       toward a point beyond the summit finish from checkpoint 36 onward. The
 *       kart still uses production physics, terrain and object collision and
 *       must set raceFinished by crossing the real finish plane. This is the
 *       closed-loop route primitive for tests/check_first_boss_progression.py,
 *       not a result or progression shortcut.
 *
 *   MDKR_SILVER_ROUTE=1
 *       Steer toward the nearest silver coin this player has not collected, when
 *       one is within MDKR_SILVER_RADIUS (default 1400) units; otherwise leave
 *       the AI line alone. The same shape as MDKR_BOSS_ROUTE and for the same
 *       reason: DKR places silver coins off the racing line on purpose, so the
 *       stock AI collects 5-6 of 8 (measured, Ancient Lake, three laps) and
 *       set_course_finish_flags()'s `silverCoinCount >= 8` write can never be
 *       observed. Steering only -- it never touches silverCoinCount or the coin
 *       objects, so a coin still counts only when obj_loop_silvercoin() sees its
 *       own interactObj->distance < 80. The radius is what keeps the detour
 *       local: divert at any range and the kart stops making checkpoint
 *       progress, so it never finishes and nothing is written at all.
 *       Used by tests/check_campaign_progression.py.
 *
 *   MDKR_SILVER_RADIUS=<units>   silver-coin detour range (default 1400)
 *
 *   MDKR_AUTOPILOT_UNSTICK=1
 *       Re-arm DKR's OWN stuck-recovery for the MDKR_AUTOPILOT racer when that
 *       recovery has become unreachable. racer_AI_pathing_inputs() reverses a
 *       stopped kart out of an obstacle (unk213 counts up to 60 while stopped,
 *       then unk214=60 kicks in reverse and unk1CA picks another AI line), but
 *       the whole block is gated on a cooldown, unk215, that is ONLY decremented
 *       while `velocity < -0.5`, i.e. while the kart is driving forwards. A kart
 *       that comes to rest with a hot cooldown therefore never re-arms: measured
 *       in Hot Top Volcano, unk215 froze at 94 and the kart sat at
 *       (-2139.4, -120.7, 1498.9) with three wheels grounded and A held for
 *       18,677 frames. That is upstream behaviour (identical in the decomp at
 *       the recorded baseline), not a port defect, so it is not changed for
 *       production; this hook only zeroes the cooldown after the autopilot kart
 *       is PROVABLY immobile (moved < 1 unit while 120 update-rate units elapse
 *       -- 60 frames -- grounded, racing and with input not blocked) and lets
 *       the game's own recovery do the driving. It never
 *       moves the kart, steers it, touches collision, laps, raceFinished or any
 *       verdict, and it is off unless asked for -- the deliberately broken
 *       collision-grid control in tests/check_first_boss_progression.py does not
 *       set it and so still fails to finish.
 *
 *   MDKR_ADVENTURE_WIN=1
 *       After controller port 1 has NATURALLY completed every lap of a default
 *       race, rotate its already-assigned finishing place to first. This is a
 *       verdict control for the Adventure persistence check, not a driving or
 *       finish shortcut: it cannot change lap, raceFinished, clock, position,
 *       physics, or any unfinished racer. The old positions remain a valid
 *       permutation (1..N), so the production finishing-order code is exercised
 *       exactly as it is for a real win.
 *
 *   MDKR_ADVENTURE_LOSS=1
 *       After the same natural finish, require a non-winning place. This is the
 *       symmetric control for builds whose instrumented racer naturally wins.
 *       If the human is first, a computer racer receives first and the human
 *       receives second; otherwise no order changes.
 *
 *   MDKR_TROPHY_ORDER="<racer ids in finish order>[/...]"
 *       At each production trophy rankings init, replace the already-complete
 *       race order with the corresponding validated permutation. Example:
 *       "0,1,2,3,4,5,6,7/1,0,2,3,4,5,6,7" makes racer 0 win round one and
 *       racer 1 win round two. This does not finish a racer and cannot write
 *       points, rankings, cinematics, trophies, or save data; all of those
 *       remain the work of menu_trophy_race_rankings_*.
 *
 * ---------------------------------------------------------------------------
 * Three things a route author must know (each cost real debugging time)
 * ---------------------------------------------------------------------------
 * 1. Doors need balloons, and the canonical route collects one.
 *    obj_loop_door() only opens on `courseFlagsPtr[courseId] & (0x10000 <<
 *    doorID)`, which it sets from `*settings->balloonsPtr >=
 *    door->balloonCount`.  Measured requirements: the hub's Dino Domain doors 1,
 *    Snowflake Mountain 2, Sherbet Island 10, Dragon Forest 16; in the Dino
 *    Domain lobby, Ancient Lake 1, Fossil Canyon 2, Jungle Falls 3, Hot Top
 *    Volcano 5, the boss doors 4 (local).  Of the hub's seven balloons only the
 *    four with challengeID == 0 are collectable: obj_init_goldenballoon() sets
 *    properties.goldenBalloon.action = 1 for a challenge balloon whose tajFlag
 *    is unset, and obj_loop_goldenballoon()'s collect branch is gated on
 *    action == 0.
 *
 *    CAVEAT, and do not rely on the opposite: an exit can be latched in FRONT of
 *    a closed door.  obj_loop_exit()'s half-plane boundary
 *    (dot(dir, racer - exit) == 0) does not coincide with the door line, and the
 *    activation radius is large (253 for the hub's Dino Domain exit, 191 for the
 *    lobby's Ancient Lake exit), so a corner exists on the near side where the
 *    test passes.  Measured: with `MDKR_DRIVE_ROUTE="0:200,500:-1004,946:E12;12:E5"`
 *    and NO balloon collected, the kart reaches the Dino Domain lobby at frame
 *    6582 and Ancient Lake at 6884 with balloons_total=0 and both relevant doors
 *    reporting open=0.  Whether real hardware allows the same corner is NOT
 *    verified here -- see docs/OPEN_ITEMS.md.  The committed route collects
 *    balloon 10 anyway, precisely so that the doors really are open (verified:
 *    the lobby's Ancient Lake door reports open=1) and the route exercises the
 *    intended path rather than the corner.
 *
 * 2. Do not bump the dialogue NPCs.  Taj (BHV_PARK_WARDEN, behaviourId 62) sits
 *    at (-197, 255, 193) in the hub and T.T. (BHV_STOPWATCH_MAN, 31) at
 *    (-9, -52, -561) in the Dino Domain lobby.  Both open a dialogue on physical
 *    contact (obj_loop_parkwarden / obj_loop_stopwatchman: INTERACT_FLAGS_PUSHING
 *    within 300 units, or Z) and then call disable_racer_input() every frame it
 *    is open, which zeroes the racer's velocity from inside update_player_racer.
 *    This hook cannot rescue that: the dialogue is advanced by input_pressed()
 *    read straight off the pad, NOT by the gCurrent* globals written here, and
 *    update_player_racer zeroes gCurrentButtonsPressed anyway whenever
 *    gRacerInputBlocked is set.  Measured: a straight line from the hub spawn to
 *    the Dino Domain exit drives into Taj and freezes at (-125.3, 149.6) with
 *    gRacerInputBlocked = 1 for 3000+ frames.  Route around them; MDKR_OBJDUMP
 *    prints where they are.
 *
 * 3. The AI-node graph is NOT a navmesh -- do not try to path-find on it.  The
 *    hub has 21 BHV_AINODE objects (MDKR_OBJDUMP=2) and they form SEVEN
 *    disconnected components: one 9-node loop on the central beach, plus a
 *    3-node triangle around each wandering object.  There is no node path from
 *    the spawn to any door, which is also why MDKR_AUTOPILOT can only lap the
 *    beach.  Cross-island travel has to be expressed as explicit waypoints; the
 *    ones in tests/check_adventure_race_loop.py are sampled from the [PACE]
 *    trace of the pre-existing open-loop hub fixture, i.e. from ground the port
 *    has already been observed to drive.
 */

#ifdef NATIVE_PORT

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* types.h first and alone: the headers below use u8/s16/f32 without including it
 * themselves, so it must not be sorted in with them. */
#include "types.h"

#include "level_object_entries.h"
#include "macros.h"
#include "PR/os_cont.h"
#include "structs.h"

#include "game.h"
#include "game_text.h"
#include "math_util.h"
#include "menu.h"
#include "objects.h"
#include "racer.h"
#include "taj_mod.h"
#include "thread3_main.h"
#include "mdkr_trace.h"
#include "mdkr_adventure.h"

extern int g_frameCounter;
extern s32 gTrophyRaceRound;
extern s32 gRaceStartTimer;
extern s8 gRacerInputBlocked;

/* Object list + the input globals update_player_racer feeds to the physics. */
extern Object **gObjPtrList;
extern s32 gObjectCount;
extern u32 gCurrentRacerInput;
extern u32 gCurrentButtonsPressed;
extern u32 gCurrentButtonsReleased;
extern s32 gCurrentStickX;
extern s32 gCurrentStickY;
extern s8 gLeadPlayerIndex;

/*
 * The campaign normally calls swap_lead_player() after P2 wins a race.  The
 * headless P2 AI lane is intentionally not a score oracle, so this bounded
 * test seam replays that already-finished handoff while the one-player hub is
 * still safe to transition.  The next, real race load is entirely retail:
 * racer creation performs the P2 lead remap and Taj binds from the swapped
 * settings row to the live P2 port.  It is not an unlock, selection, scoring,
 * or physics override.
 */
static void mdkr_taj_p2_lead_prepare(const Object_Racer *racer) {
    static s32 initialized;
    static s32 enabled;
    static s32 handedOff;
    const char *value;

    if (!initialized) {
        initialized = TRUE;
        value = getenv("MDKR_TAJ_P2_LEAD_HANDOFF");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (!enabled || handedOff || racer == NULL ||
        !is_in_two_player_adventure() ||
        gLeadPlayerIndex != PLAYER_ONE || racer->playerIndex != PLAYER_ONE ||
        !taj_mod_player_selected(PLAYER_TWO) ||
        taj_mod_player_selected(PLAYER_ONE)) {
        return;
    }
    handedOff = TRUE;
    swap_lead_player();
    MDKR_TRACE("taj_adv_handoff: source=retail_swap lead=%d "
               "settings_taj0=%d settings_taj1=%d",
               (int) gLeadPlayerIndex,
               taj_mod_player_selected(PLAYER_ONE),
               taj_mod_player_selected(PLAYER_TWO));
}

/*
 * P2-led Adventure is the one place DKR intentionally decouples the stable
 * settings rows from the live controller ports.  This witness is test-only:
 * it runs after obj_init_racer() has bound both masks, observes the exact
 * post-swap tuple once, and changes no gameplay state.  The required tuple
 * for a P2-selected Taj after the retail lead handoff is:
 *
 *   lead=1 settings=[Taj, ordinary] live=[ordinary, Taj]
 *
 * Keeping the ids in the row makes a virtual character-id leak falsifiable as
 * well: every retail consumer must continue to see 0..9 (Taj uses donor 9).
 */
static void mdkr_taj_p2_lead_trace(const Object_Racer *racer) {
    static s32 initialized;
    static s32 enabled;
    static s32 reported;
    Settings *settings;
    s32 idsRetail;
    const char *value;

    if (!initialized) {
        initialized = TRUE;
        value = getenv("MDKR_TAJ_P2_LEAD_TRACE");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (!enabled || reported || racer == NULL || !is_in_two_player_adventure() ||
        gLeadPlayerIndex != PLAYER_TWO || racer->racerIndex != PLAYER_ONE ||
        racer->playerIndex != PLAYER_TWO ||
        !taj_mod_racer_is_taj(PLAYER_TWO)) {
        return;
    }
    settings = get_settings();
    if (settings == NULL) {
        return;
    }
    idsRetail = settings->racers[PLAYER_ONE].character >= CHARACTER_KRUNCH &&
                settings->racers[PLAYER_ONE].character <= CHARACTER_DIDDY &&
                settings->racers[PLAYER_TWO].character >= CHARACTER_KRUNCH &&
                settings->racers[PLAYER_TWO].character <= CHARACTER_DIDDY &&
                racer->characterId >= CHARACTER_KRUNCH &&
                racer->characterId <= CHARACTER_DIDDY;
    reported = TRUE;
    MDKR_TRACE("taj_adv_identity: lead=%d settings_taj0=%d settings_taj1=%d "
               "live_taj0=%d live_taj1=%d racer_slot=%d racer_port=%d "
               "ids=%d,%d,%d retail_ids=%d",
               (int)gLeadPlayerIndex,
               taj_mod_player_selected(PLAYER_ONE),
               taj_mod_player_selected(PLAYER_TWO),
               taj_mod_racer_is_taj(PLAYER_ONE),
               taj_mod_racer_is_taj(PLAYER_TWO),
               (int)racer->racerIndex, (int)racer->playerIndex,
               (int)settings->racers[PLAYER_ONE].character,
               (int)settings->racers[PLAYER_TWO].character,
               (int)racer->characterId, idsRetail);
}

/**
 * Test-only finishing-verdict control used by check_adventure_race_loop.py.
 *
 * The headless AI reaches and finishes all three Ancient Lake laps reliably,
 * but its natural place differs between Debug, optimized, and sanitizer builds.
 * Once the production finish detector has assigned that place, this hook can
 * request either verdict before race_check_finish() builds gRacersByPosition
 * and decides whether to persist a reward. Requiring the natural lap and
 * raceFinished predicates keeps it from manufacturing a finish.
 */
void mdkr_adventure_force_verdict(Object *humanObj, Object **racers, s32 numRacers) {
    static s32 initialized;
    static s32 requestedVerdict;
    static s32 reported;
    const char *winValue;
    const char *lossValue;
    LevelHeader *header;
    Object_Racer *human;
    Object_Racer *lossWinner;
    s32 oldPosition;
    s32 i;

    if (!initialized) {
        initialized = TRUE;
        winValue = getenv("MDKR_ADVENTURE_WIN");
        lossValue = getenv("MDKR_ADVENTURE_LOSS");
        if (winValue != NULL && winValue[0] != '\0' && winValue[0] != '0') {
            requestedVerdict = 1;
        } else if (lossValue != NULL && lossValue[0] != '\0' && lossValue[0] != '0') {
            requestedVerdict = -1;
        }
    }
    if (requestedVerdict == 0 || humanObj == NULL || humanObj->racer == NULL ||
        racers == NULL || numRacers <= 0 || is_in_tracks_mode()) {
        return;
    }

    header = level_header();
    human = humanObj->racer;
    if (header == NULL || header->race_type != RACETYPE_DEFAULT ||
        !human->raceFinished || human->lap < header->laps ||
        human->racerIndex != PLAYER_ONE || human->finishPosition < 1 ||
        human->finishPosition > numRacers) {
        return;
    }

    oldPosition = human->finishPosition;
    if (requestedVerdict > 0 && oldPosition > 1) {
        for (i = 0; i < numRacers; i++) {
            Object_Racer *racer;
            if (racers[i] == NULL || racers[i]->racer == NULL ||
                racers[i] == humanObj) {
                continue;
            }
            racer = racers[i]->racer;
            if (racer->raceFinished && racer->finishPosition >= 1 &&
                racer->finishPosition < oldPosition) {
                racer->finishPosition++;
            }
        }
        human->finishPosition = 1;
    } else if (requestedVerdict < 0 && oldPosition == 1) {
        lossWinner = NULL;
        for (i = 0; i < numRacers; i++) {
            Object_Racer *racer;
            if (racers[i] == NULL || racers[i]->racer == NULL ||
                racers[i] == humanObj) {
                continue;
            }
            racer = racers[i]->racer;
            if (racer->raceFinished && racer->finishPosition == 2) {
                lossWinner = racer;
                break;
            }
        }
        if (lossWinner == NULL) {
            for (i = 0; i < numRacers; i++) {
                Object_Racer *racer;
                if (racers[i] == NULL || racers[i]->racer == NULL ||
                    racers[i] == humanObj) {
                    continue;
                }
                racer = racers[i]->racer;
                if (racer->playerIndex == PLAYER_COMPUTER) {
                    lossWinner = racer;
                    break;
                }
            }
        }
        if (lossWinner == NULL) {
            return;
        }
        lossWinner->raceFinished = TRUE;
        lossWinner->finishPosition = 1;
        human->finishPosition = 2;
    }
    if (!reported && mdkr_trace_enabled()) {
        reported = TRUE;
        mdkr_trace("adventureforceverdict: requested=%s natural=%d final=%d "
                   "lap=%d (racerIndex=%d playerIndex=%d) @frame~%d",
                   requestedVerdict > 0 ? "win" : "loss", (int) oldPosition,
                   (int) human->finishPosition, (int) human->lap,
                   (int) human->racerIndex, (int) human->playerIndex,
                   g_frameCounter);
    }
}

/* ------------------------------------------------------------------ config */

#define MDKR_ADV_MAX_LEVELS 6
/* 48, not 10: a closed-loop tour of Timber's Island needs its waypoints close
 * enough together that the straight line BETWEEN two of them still follows
 * drivable ground -- the hook drives at the next point, so it is the CHORD that
 * has to be drivable, not just the sampled positions. Two runs' worth of
 * evidence, both on tests/check_adventure_hub.py's HUB_TOUR_ROUTE:
 *   - 4500-unit spacing (9 points): a chord passed 158 units from Taj
 *     (BHV_PARK_WARDEN), whose dialogue opens on contact within 300 units and
 *     then calls disable_racer_input() every frame. The kart travelled 224 units
 *     in 6133 frames.
 *   - 2200-unit spacing (20 points): the chord from (-1180, 691) to (890, 921)
 *     cuts across the island's middle; the kart wedged at (79, 243, 766) with
 *     velocity 0 and sat there for 5000 frames, the stall/reverse logic firing
 *     every 131 frames without freeing it.
 * At ~1000 units the polyline follows the traced line closely enough to stay on
 * the ground it was sampled from. 48 gives that room with margin. */
#define MDKR_ADV_MAX_STEPS 48

enum MdkrAdvStepKind {
    MDKR_ADV_POINT,  /* x,z          -- completes inside sAdvWpRadius       */
    MDKR_ADV_EXIT,   /* E<destMapId> -- retires when the level changes      */
    MDKR_ADV_BALLOON,/* B<balloonID> -- completes on the collected flag     */
    MDKR_ADV_TROPHY  /* T            -- production cabinet collision         */
};

typedef struct MdkrAdvStep {
    s32 kind;
    s32 id; /* destMapId / balloonID */
    f32 x;
    f32 z;
} MdkrAdvStep;

typedef struct MdkrAdvLevel {
    s32 levelId;
    s32 count;
    MdkrAdvStep step[MDKR_ADV_MAX_STEPS];
} MdkrAdvLevel;

static s32 sAdvInited = 0;
static s32 sAdvLevelCount = 0;
static MdkrAdvLevel sAdvLevels[MDKR_ADV_MAX_LEVELS];
static f32 sAdvWpRadius = 220.0f;
static s32 sAdvObjdump = 0;
static s32 sAdvVerbose = 0;
static s32 sAdvBossRoute = 0;
static s32 sAdvBossRouteAnnounced = 0;
static s32 sAdvSilverRoute = 0;
static f32 sAdvSilverRadius = 1400.0f;
static s32 sAdvSilverSeeking = 0;

#define MDKR_TRICKY_ONE_LEVEL 38
#define MDKR_TRICKY_SUMMIT_CHECKPOINT 36
#define MDKR_TRICKY_FINISH_TARGET_X 350.0f
#define MDKR_TRICKY_FINISH_TARGET_Z 1200.0f

/* Step progress is per configured level and PERSISTS across leaving and
 * re-entering that level, because the Adventure loop comes back: the world lobby
 * is visited once on the way into a race and again on the way out, and the two
 * visits want different steps.  An EXIT step is marked done when the level
 * actually changes while that step is the active one, which is the only
 * observable "this exit was used".  The steering state below is what resets. */
static s32 sAdvStepIdx[MDKR_ADV_MAX_LEVELS];
static s32 sAdvLevel = -2;
static s32 sAdvPrevSlot = -1;
static f32 sAdvBestDist = 1.0e30f;
static s32 sAdvStall = 0;
static s32 sAdvReverse = 0;

static f32 mdkr_adv_num(const char **p) {
    char *end = NULL;
    f32 v = (f32) strtod(*p, &end);
    if (end != NULL) {
        *p = end;
    }
    return v;
}

/* MDKR_DRIVE_ROUTE="<lvl>:<step>[:<step>...][;<lvl>:...]" */
static void mdkr_adv_parse_route(const char *s) {
    while (*s != '\0' && sAdvLevelCount < MDKR_ADV_MAX_LEVELS) {
        MdkrAdvLevel *lv;
        while (*s == ';' || *s == ' ') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        lv = &sAdvLevels[sAdvLevelCount];
        lv->count = 0;
        lv->levelId = (s32) mdkr_adv_num(&s);
        if (*s != ':') {
            break; /* malformed -- stop rather than guess */
        }
        while (*s == ':' && lv->count < MDKR_ADV_MAX_STEPS) {
            MdkrAdvStep *st = &lv->step[lv->count];
            s++;
            st->x = 0.0f;
            st->z = 0.0f;
            st->id = -1;
            if (*s == 'E' || *s == 'e') {
                s++;
                st->kind = MDKR_ADV_EXIT;
                st->id = (s32) mdkr_adv_num(&s);
            } else if (*s == 'B' || *s == 'b') {
                s++;
                st->kind = MDKR_ADV_BALLOON;
                st->id = (s32) mdkr_adv_num(&s);
            } else if (*s == 'T' || *s == 't') {
                s++;
                st->kind = MDKR_ADV_TROPHY;
            } else {
                st->kind = MDKR_ADV_POINT;
                st->x = mdkr_adv_num(&s);
                if (*s != ',') {
                    break;
                }
                s++;
                st->z = mdkr_adv_num(&s);
            }
            lv->count++;
        }
        sAdvLevelCount++;
    }
}

static void mdkr_adv_init(void) {
    const char *e;
    if (sAdvInited) {
        return;
    }
    sAdvInited = 1;
    e = getenv("MDKR_OBJDUMP");
    /* 1 = exits/doors/balloons/NPCs, 2 = also every AI node + its adjacency. */
    sAdvObjdump = (e != NULL) ? atoi(e) : 0;
    e = getenv("MDKR_DRIVE_ROUTE");
    if (e != NULL && e[0] != '\0') {
        mdkr_adv_parse_route(e);
    }
    e = getenv("MDKR_DRIVE_WPR");
    if (e != NULL && e[0] != '\0') {
        sAdvWpRadius = (f32) atof(e);
    }
    e = getenv("MDKR_DRIVE_VERBOSE");
    sAdvVerbose = (e != NULL && e[0] == '1') ? 1 : 0;
    e = getenv("MDKR_BOSS_ROUTE");
    sAdvBossRoute = (e != NULL && e[0] == '1') ? 1 : 0;
    e = getenv("MDKR_SILVER_ROUTE");
    sAdvSilverRoute = (e != NULL && e[0] == '1') ? 1 : 0;
    e = getenv("MDKR_SILVER_RADIUS");
    if (e != NULL && e[0] != '\0') {
        sAdvSilverRadius = (f32) atof(e);
    }
}

/* ------------------------------------------------------------------- dump */

/**
 * Print the level's exit / door / balloon / NPC objects.
 *
 * All LevelObjectEntry_Exit, _Door and _GoldenBalloon body fields are single
 * bytes, so none of them is touched by mdkr_objmap_swap_entry_body()'s
 * per-behaviour halfword swapping -- there is nothing there to swap.  x/y/z come
 * from the common path, which is swapped for every entry.
 */
static void mdkr_adv_dump_level(s32 levelId) {
    Settings *settings = get_settings();
    s32 nExit = 0, nDoor = 0, nBalloon = 0, nNpc = 0, nNode = 0;
    s32 i;

    mdkr_trace("objdump: level=%d objects=%d balloons_total=%d world=%d @frame~%d", (int) levelId, (int) gObjectCount,
               (settings != NULL && settings->balloonsPtr != NULL) ? (int) settings->balloonsPtr[0] : -1,
               (settings != NULL) ? (int) settings->worldId : -1, g_frameCounter);
    for (i = 0; i < gObjectCount; i++) {
        Object *obj = gObjPtrList[i];
        if (obj == NULL || obj->level_entry == NULL) {
            continue;
        }
        if (obj->behaviorId == BHV_EXIT) {
            LevelObjectEntry_Exit *entry = &obj->level_entry->exit;
            Object_Exit *ex = obj->exit;
            mdkr_trace("objdump:   EXIT    i=%d pos=(%.1f, %.1f, %.1f) dest=%d radius=%d dir=(%.3f, %.3f) "
                       "rotDiff=%.1f boss=%d owSpawn=%d retSpawn=%d",
                       (int) i, obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                       (int) entry->destinationMapId, (int) ex->radius, ex->directionX, ex->directionZ,
                       ex->rotationDiff, (int) ex->bossFlag, (int) entry->overworldSpawnIndex,
                       (int) entry->returnSpawnIndex);
            nExit++;
        } else if (obj->behaviorId == BHV_DOOR || obj->behaviorId == BHV_TT_DOOR) {
            LevelObjectEntry_Door *entry = &obj->level_entry->door;
            Object_Door *dr = obj->door;
            mdkr_trace("objdump:   DOOR    i=%d pos=(%.1f, %.1f, %.1f) bhv=%d doorID=%d balloons=%d type=%d "
                       "openRadius=%d textID=%d keyID=%d levelID=%d localBalloons=%d objectID=%d open=%d",
                       (int) i, obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                       (int) obj->behaviorId, (int) dr->doorID, (int) dr->balloonCount, (int) dr->doorType,
                       (int) dr->radius, (int) dr->textID, (int) dr->keyID, (int) entry->levelID,
                       (int) entry->localBalloons, (int) entry->common.objectID,
                       (settings != NULL && dr->doorID >= 0)
                           ? ((settings->courseFlagsPtr[settings->courseId] & (0x10000 << dr->doorID)) != 0)
                           : -1);
            nDoor++;
        } else if (obj->behaviorId == BHV_GOLDEN_BALLOON) {
            LevelObjectEntry_GoldenBalloon *entry = &obj->level_entry->goldenBalloon;
            mdkr_trace("objdump:   BALLOON i=%d pos=(%.1f, %.1f, %.1f) balloonID=%d challengeID=%d action=%d",
                       (int) i, obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                       (int) entry->balloonID, (int) entry->challengeID,
                       (int) obj->properties.goldenBalloon.action);
            nBalloon++;
        } else if (obj->behaviorId == BHV_STOPWATCH_MAN || obj->behaviorId == BHV_PARK_WARDEN ||
                   obj->behaviorId == BHV_PARK_WARDEN_2 || obj->behaviorId == BHV_TROPHY_CABINET) {
            /* T.T., Taj and the trophy cabinet all call disable_racer_input()
             * while their dialogue is open, and bumping them is what opens it.
             * A route has to steer clear, so it has to know where they are. */
            mdkr_trace("objdump:   NPC     i=%d pos=(%.1f, %.1f, %.1f) bhv=%d", (int) i, obj->trans.x_position,
                       obj->trans.y_position, obj->trans.z_position, (int) obj->behaviorId);
            nNpc++;
        } else if (obj->behaviorId == BHV_AINODE) {
            LevelObjectEntry_AiNode *entry = &obj->level_entry->aiNode;
            if (sAdvObjdump >= 2) {
                mdkr_trace("objdump:   AINODE  i=%d pos=(%.1f, %.1f, %.1f) nodeID=%d adj=%d,%d,%d,%d elev=%d",
                           (int) i, obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                           (int) entry->nodeID, (int) entry->adjacent[0], (int) entry->adjacent[1],
                           (int) entry->adjacent[2], (int) entry->adjacent[3], (int) entry->elevation);
            }
            nNode++;
        }
    }
    mdkr_trace("objdump: level=%d exits=%d doors=%d balloons=%d npcs=%d ainodes=%d", (int) levelId, (int) nExit,
               (int) nDoor, (int) nBalloon, (int) nNpc, (int) nNode);
}

/* ------------------------------------------------------------------ drive */

static Object *mdkr_adv_find(s32 behaviorId, s32 wantId) {
    s32 i;
    for (i = 0; i < gObjectCount; i++) {
        Object *obj = gObjPtrList[i];
        s32 gotId;
        if (obj == NULL || obj->behaviorId != behaviorId || obj->level_entry == NULL) {
            continue;
        }
        if (behaviorId == BHV_TROPHY_CABINET) {
            return obj;
        }
        gotId = (behaviorId == BHV_EXIT) ? (s32) obj->level_entry->exit.destinationMapId
                                         : (s32) obj->level_entry->goldenBalloon.balloonID;
        if (gotId == wantId) {
            return obj;
        }
    }
    return NULL;
}

static s32 mdkr_adv_slot_for(s32 levelId) {
    s32 i;
    for (i = 0; i < sAdvLevelCount; i++) {
        if (sAdvLevels[i].levelId == levelId) {
            return i;
        }
    }
    return -1;
}

/* A golden balloon is collected when obj_loop_goldenballoon has set its bit in
 * courseFlagsPtr[courseId] -- the same flag the door check reads. */
static s32 mdkr_adv_balloon_collected(Settings *settings, s32 balloonID) {
    if (settings == NULL || settings->courseFlagsPtr == NULL || balloonID < 0 || balloonID > 14) {
        return 0;
    }
    return (settings->courseFlagsPtr[settings->courseId] & (0x10000 << balloonID)) != 0;
}

/**
 * Aim the stick at a point (dx, dz) units away and hold the throttle.
 *
 * This is DKR's own "steer toward a world position" block, taken from
 * racer_AI_pathing_inputs() (game/src/racer.c:765, the AI's steer-to-next-node):
 *
 *     temp   = ((arctan2_f(xDiff, zDiff)) - 0x8000) & 0xFFFF;
 *     var_v1 = temp - (aiRacer->steerVisualRotation & 0xFFFF);
 *     WRAP(var_v1, -0x8000, 0x8000);
 *     gCurrentStickX = -var_v1 >> 4;
 *
 * The `- 0x8000` is load-bearing and is why a first attempt drove the kart 180
 * degrees away from the target and then straight on for ever (the stable fixed
 * point of inverted steering): steerVisualRotation is a HALF TURN out of phase
 * with the geometric heading.  The forward vector the physics integrates is
 * racer->ox1/oz1, i.e. local +z through the object matrix, which points at
 * steerVisualRotation - 0x8000.
 *
 * racer_enter_door() looks like a counter-example -- it compares
 * arctan2_f(exit->directionX, exit->directionZ) against steerVisualRotation with
 * no offset -- but it aligns against the exit's own stored facing vector, which
 * is already anti-parallel to the direction of travel through the door.  Copying
 * that idiom for a raw position delta is exactly what produced the inversion.
 */
static void mdkr_adv_steer(Object_Racer *racer, f32 dx, f32 dz, s32 reverse) {
    s32 want = (s32) ((((u16) arctan2_f(dx, dz)) - 0x8000) & 0xFFFF);
    s32 angle = want - (s32) (racer->steerVisualRotation & 0xFFFF);
    WRAP(angle, -0x8000, 0x8000);
    angle = -angle >> 4;
    if (reverse) {
        /* Backing out of a wall: mirror the steering so the nose swings toward
         * the target instead of straight back into the obstacle. */
        angle = -angle;
    }
    if (angle > 80) {
        angle = 80;
    }
    if (angle < -80) {
        angle = -80;
    }
    gCurrentStickX = angle;
    gCurrentStickY = reverse ? -80 : 0; /* DKR only reverses with the stick back */
    gCurrentButtonsPressed = 0;
    gCurrentButtonsReleased = 0;
    gCurrentRacerInput = reverse ? B_BUTTON : A_BUTTON;
}

/**
 * The hook.  Called once per frame from update_player_racer() for the human
 * racer, after the normal input dispatch and after the MDKR_AUTOPILOT hook.
 * With no environment variable set this costs one cached getenv plus an integer
 * compare and returns without touching anything.
 */
void mdkr_adventure_drive(Object *obj, Object_Racer *racer, s32 updateRate) {
    Settings *settings;
    MdkrAdvLevel *lv;
    MdkrAdvStep *st;
    Object *target;
    s32 levelId;
    s32 slot;
    f32 tx, tz, dx, dz, dist;

    mdkr_taj_p2_lead_prepare(racer);
    mdkr_taj_p2_lead_trace(racer);
    mdkr_adv_init();
    if (sAdvLevelCount == 0 && !sAdvObjdump && !sAdvBossRoute && !sAdvSilverRoute) {
        return;
    }
    settings = get_settings();
    if (settings == NULL) {
        return;
    }
    /* settings->courseId is the level DKR itself considers current: level_load()
     * assigns it from the (possibly boss-remapped) levelId it actually loaded. */
    levelId = settings->courseId;

    if (levelId != sAdvLevel) {
        /* Leaving a level while an EXIT step was active means that exit was
         * taken, so retire it -- that is what lets the world lobby drive INTO
         * the race on the way in and back to Timber's Island on the way out. */
        if (sAdvPrevSlot >= 0 && sAdvStepIdx[sAdvPrevSlot] < sAdvLevels[sAdvPrevSlot].count &&
            sAdvLevels[sAdvPrevSlot].step[sAdvStepIdx[sAdvPrevSlot]].kind == MDKR_ADV_EXIT) {
            if (mdkr_trace_enabled()) {
                mdkr_trace("drive: level=%d step %d: exit to %d TAKEN (now in level %d) @frame~%d",
                           (int) sAdvLevels[sAdvPrevSlot].levelId, (int) sAdvStepIdx[sAdvPrevSlot],
                           (int) sAdvLevels[sAdvPrevSlot].step[sAdvStepIdx[sAdvPrevSlot]].id, (int) levelId,
                           g_frameCounter);
            }
            sAdvStepIdx[sAdvPrevSlot]++;
        }
        sAdvLevel = levelId;
        sAdvPrevSlot = mdkr_adv_slot_for(levelId);
        sAdvBestDist = 1.0e30f;
        sAdvStall = 0;
        sAdvReverse = 0;
        sAdvBossRouteAnnounced = 0;
        if (sAdvObjdump && mdkr_trace_enabled()) {
            mdkr_adv_dump_level(levelId);
        }
    }

    /* Once an exit has latched the racer, racer_enter_door() owns the input and
     * drives the final approach itself.  Never fight it. */
    if (racer->exitObj != NULL) {
        return;
    }

    /*
     * Tricky 1's stock AI line is a knife edge after production object-model
     * collision is enabled: one legitimate nine-frame brush 4,150 frames
     * earlier moves the human 2.5 units, and the line then crosses the summit
     * finish just outside its narrow X extent.  This fixture recovery does not
     * move the racer, alter collision, set raceFinished, or choose a verdict.
     * It only replaces the AI's stick input after the penultimate checkpoint
     * with a target beyond the finish plane, leaving the kart to cross it under
     * normal physics.  MDKR_BOSS_WIN remains a separate post-finish verdict
     * control and cannot fire unless this route naturally reaches the finish.
     */
    if (sAdvBossRoute && levelId == MDKR_TRICKY_ONE_LEVEL &&
        racer->courseCheckpoint >= MDKR_TRICKY_SUMMIT_CHECKPOINT && !racer->raceFinished) {
        if (!sAdvBossRouteAnnounced && mdkr_trace_enabled()) {
            mdkr_trace("bossroute: level=38 checkpoint=%d steering through summit finish @frame~%d",
                       (int) racer->courseCheckpoint, g_frameCounter);
            sAdvBossRouteAnnounced = 1;
        }
        mdkr_adv_steer(racer, MDKR_TRICKY_FINISH_TARGET_X - obj->trans.x_position,
                       MDKR_TRICKY_FINISH_TARGET_Z - obj->trans.z_position, 0);
        return;
    }

    /*
     * MDKR_SILVER_ROUTE -- the silver-coin analogue of MDKR_BOSS_ROUTE, and for
     * the same reason: the stock AI line cannot complete the objective.
     *
     * DKR's silver coins are deliberately placed OFF the racing line -- that is
     * what makes the challenge a challenge -- so racer_AI_pathing_inputs(), which
     * drives MDKR_AUTOPILOT, sweeps up only the coins that happen to sit near the
     * spline. Measured on Ancient Lake over three laps: 5 and 6 of 8. Since
     * set_course_finish_flags() writes RACE_CLEARED_SILVER_COINS only at
     * silverCoinCount >= 8, an autopilot run can never witness that write, and
     * the whole silver-coin seam stays unobservable.
     *
     * This supplies STEERING ONLY, exactly like the Tricky 1 summit recovery
     * above. It does not move the kart, touch collision, set laps, checkpoints,
     * raceFinished or any verdict, and -- the point -- it does not touch
     * silverCoinCount or the coin objects. A coin still counts only when
     * obj_loop_silvercoin() sees the production interactObj->distance < 80 and
     * increments the counter itself, so what is being witnessed remains the
     * game's own collection and persistence path.
     *
     * The detour is deliberately LOCAL (default 1400 units, MDKR_SILVER_RADIUS):
     * diverting at any range would abandon the AI line entirely and the kart
     * would stop making checkpoint progress, so laps -- and therefore the finish
     * that triggers the flag write -- would never happen. Within the radius the
     * kart peels off, collects, and is handed straight back to the AI. Three laps
     * give each coin three chances.
     */
    if (sAdvSilverRoute && !racer->raceFinished) {
        Object *coin = NULL;
        f32 best = sAdvSilverRadius * sAdvSilverRadius;
        s32 collectedBit = 1 << racer->playerIndex; /* SILVER_COIN_COLLECTED << playerIndex */
        s32 i;
        for (i = 0; i < gObjectCount; i++) {
            Object *cand = gObjPtrList[i];
            f32 cx, cz, d2;
            if (cand == NULL) {
                continue;
            }
            if (cand->behaviorId != BHV_SILVER_COIN && cand->behaviorId != BHV_SILVER_COIN_2) {
                continue;
            }
            /* INACTIVE coins free themselves at init, so anything still in the
             * list is live; skip only the ones this player already has. */
            if (cand->properties.silverCoin.action & collectedBit) {
                continue;
            }
            cx = cand->trans.x_position - obj->trans.x_position;
            cz = cand->trans.z_position - obj->trans.z_position;
            d2 = (cx * cx) + (cz * cz);
            if (d2 < best) {
                best = d2;
                coin = cand;
            }
        }
        if (coin != NULL) {
            if (!sAdvSilverSeeking && mdkr_trace_enabled()) {
                mdkr_trace("silverroute: level=%d seeking coin at (%.1f, %.1f) @frame~%d", (int) levelId,
                           coin->trans.x_position, coin->trans.z_position, g_frameCounter);
            }
            sAdvSilverSeeking = 1;
            mdkr_adv_steer(racer, coin->trans.x_position - obj->trans.x_position,
                           coin->trans.z_position - obj->trans.z_position, 0);
            return;
        }
        sAdvSilverSeeking = 0;
    }

    slot = sAdvPrevSlot;
    if (slot < 0) {
        return; /* unrouted level -- leave the script / autopilot alone */
    }
    lv = &sAdvLevels[slot];

    /* Advance past every completed step first, so a balloon collected while
     * steering at it hands over to the next step on the same frame. */
    while (sAdvStepIdx[slot] < lv->count) {
        st = &lv->step[sAdvStepIdx[slot]];
        if (st->kind == MDKR_ADV_BALLOON && mdkr_adv_balloon_collected(settings, st->id)) {
            if (mdkr_trace_enabled()) {
                mdkr_trace("drive: level=%d step %d: balloon %d COLLECTED (total=%d) @frame~%d", (int) levelId,
                           (int) sAdvStepIdx[slot], (int) st->id,
                           (settings->balloonsPtr != NULL) ? (int) settings->balloonsPtr[0] : -1, g_frameCounter);
            }
        } else if (st->kind == MDKR_ADV_POINT) {
            f32 wx = st->x - obj->trans.x_position;
            f32 wz = st->z - obj->trans.z_position;
            if (sqrtf((wx * wx) + (wz * wz)) >= sAdvWpRadius) {
                break;
            }
            if (mdkr_trace_enabled()) {
                mdkr_trace("drive: level=%d step %d: waypoint (%.0f, %.0f) reached @frame~%d", (int) levelId,
                           (int) sAdvStepIdx[slot], st->x, st->z, g_frameCounter);
            }
        } else {
            break; /* EXIT retires on the level change; an uncollected balloon holds */
        }
        sAdvStepIdx[slot]++;
        sAdvBestDist = 1.0e30f;
        sAdvStall = 0;
        sAdvReverse = 0;
    }
    if (sAdvStepIdx[slot] >= lv->count) {
        return; /* route exhausted -- hand the kart back */
    }

    st = &lv->step[sAdvStepIdx[slot]];
    target = NULL;
    if (st->kind == MDKR_ADV_EXIT) {
        target = mdkr_adv_find(BHV_EXIT, st->id);
    } else if (st->kind == MDKR_ADV_BALLOON) {
        target = mdkr_adv_find(BHV_GOLDEN_BALLOON, st->id);
    } else if (st->kind == MDKR_ADV_TROPHY) {
        target = mdkr_adv_find(BHV_TROPHY_CABINET, -1);
    }
    if (st->kind != MDKR_ADV_POINT) {
        if (target == NULL) {
            return; /* object not in this level (or already freed) */
        }
        tx = target->trans.x_position;
        tz = target->trans.z_position;
    } else {
        tx = st->x;
        tz = st->z;
    }

    dx = tx - obj->trans.x_position;
    dz = tz - obj->trans.z_position;
    dist = sqrtf((dx * dx) + (dz * dz));

    /* Stall detection, purely frame-count driven so it stays deterministic. */
    if (dist < sAdvBestDist - 1.0f) {
        sAdvBestDist = dist;
        sAdvStall = 0;
    } else {
        sAdvStall += updateRate;
    }
    if (sAdvReverse > 0) {
        sAdvReverse -= updateRate;
        if (sAdvReverse <= 0) {
            sAdvReverse = 0;
            sAdvBestDist = dist;
            sAdvStall = 0;
        }
    } else if (sAdvStall > 90) {
        sAdvReverse = 40;
        sAdvStall = 0;
        if (sAdvVerbose && mdkr_trace_enabled()) {
            mdkr_trace("drive: level=%d step %d stalled at %.1f units, reversing @frame~%d", (int) levelId,
                       (int) sAdvStepIdx[slot], dist, g_frameCounter);
        }
    }

    mdkr_adv_steer(racer, dx, dz, sAdvReverse > 0);

    if (sAdvVerbose && mdkr_trace_enabled() && (g_frameCounter % 30) == 0) {
        f32 dy = (st->kind != MDKR_ADV_POINT) ? (target->trans.y_position - obj->trans.y_position) : 0.0f;
        mdkr_trace("drive: level=%d step=%d kind=%d id=%d target=(%.0f, %.0f) pos=(%.0f, %.0f, %.0f) dist=%.1f "
                   "dy=%.1f stick=%d rev=%d vel=%.1f blk=%d",
                   (int) levelId, (int) sAdvStepIdx[slot], (int) st->kind, (int) st->id, tx, tz, obj->trans.x_position,
                   obj->trans.y_position, obj->trans.z_position, dist, dy, (int) gCurrentStickX, (int) sAdvReverse,
                   racer->velocity, (int) textbox_visible());
    }
}

/* ------------------------------------------ autopilot stuck-recovery re-arm */

/* A kart that has moved less than this in the plane while MDKR_UNSTICK_FRAMES
 * update-rate units accumulate (60 frames at the fixture's rate of 2) is not
 * driving, it is wedged: the measured Hot Top Volcano wedge moved 0.000 units
 * over 18,677 frames, while the slowest genuine driving in the same fixture --
 * the wall-scrape recovery on the way to the boss door -- still covers tens of
 * units in that window. */
#define MDKR_UNSTICK_EPSILON 1.0f
#define MDKR_UNSTICK_FRAMES 120
#define MDKR_UNSTICK_PLAYERS 4

typedef struct MdkrAutopilotUnstickState {
    f32 anchorX;
    f32 anchorZ;
    s32 immobile;
    s32 initialized;
} MdkrAutopilotUnstickState;

/**
 * Re-arm racer_AI_pathing_inputs()'s own stuck-recovery for the autopilot racer.
 *
 * The recovery block is gated on `racer->unk215 == 0`, and unk215 is only
 * decremented while `racer->velocity < -0.5` -- while the kart drives forwards.
 * A kart that stops with a hot cooldown can therefore never satisfy the gate
 * again, so the game's reverse-out never runs. That is exactly what the vendored
 * decomp does (verified against the baseline in .decomp-baseline), so the
 * production simulation is left alone and this test hook zeroes the cooldown
 * only after the kart is provably immobile. Everything after that is the game's
 * own code: unk213 accumulates, unk214 kicks the reverse, unk1CA picks another
 * AI line. This never writes a position, a lap, raceFinished or a verdict.
 *
 * Off unless MDKR_AUTOPILOT_UNSTICK is set, so no other fixture's trajectory can
 * move -- notably the deliberately broken collision-grid control, which must
 * keep failing to finish.
 *
 *   MDKR_AUTOPILOT_UNSTICK=1        any level (the original contract)
 *   MDKR_AUTOPILOT_UNSTICK=L<id>    only while courseId == <id>
 *
 * The scoped form exists because "this hook only ever fires at Hot Top Volcano"
 * was a property enforced by ONE check's assertion
 * (tests/check_first_boss_progression.py's `off_course` block) rather than by
 * the hook, while tests/check_race_multiplayer.py enables the unscoped form on
 * Ancient Lake with no equivalent assertion. Scoping is opt-in rather than
 * mandatory precisely because that multiplayer use is deliberate: the point is
 * that a fixture which knows where the wedge is can now say so to the guard,
 * and a new wedge site elsewhere then fails to be rescued instead of being
 * silently absorbed.
 */
void mdkr_autopilot_unstick(Object *obj, Object_Racer *racer, s32 updateRate) {
    static s32 sUnstickOn = -1;
    static s32 sUnstickLevel = -1;
    static MdkrAutopilotUnstickState sState[MDKR_UNSTICK_PLAYERS];
    MdkrAutopilotUnstickState *state;
    Settings *settings;
    s32 levelId;
    s32 playerIndex;
    f32 dx;
    f32 dz;

    if (sUnstickOn < 0) {
        const char *e = getenv("MDKR_AUTOPILOT_UNSTICK");
        sUnstickOn = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        if (sUnstickOn && (e[0] == 'L' || e[0] == 'l') && e[1] != '\0') {
            sUnstickLevel = atoi(e + 1);
        }
    }
    if (!sUnstickOn) {
        return;
    }

    /* update_player_racer restores the human identity before calling this
     * hook. Keep independent anchors for every local player: a shared anchor
     * is overwritten by the next racer's distant position each tick and can
     * therefore never detect one wedged kart in split-screen automation. */
    playerIndex = racer->playerIndex;
    if (playerIndex < 0 || playerIndex >= MDKR_UNSTICK_PLAYERS) {
        return;
    }
    state = &sState[playerIndex];
    if (!state->initialized) {
        state->anchorX = obj->trans.x_position;
        state->anchorZ = obj->trans.z_position;
        state->immobile = 0;
        state->initialized = TRUE;
    }

    settings = get_settings();
    if (settings == NULL) {
        return;
    }
    levelId = settings->courseId;

    /* Two scopes this hook deliberately does NOT own:
     *  - a level with an MDKR_DRIVE_ROUTE, because the waypoint driver already
     *    carries its own deterministic stall-and-reverse for exactly this;
     *  - a kart that has not passed a checkpoint yet, which is how a racer
     *    parked in a cutscene or on a results screen presents. Restricting to a
     *    kart that is demonstrably mid-race removes every one of the 30-odd
     *    idle firings the unrestricted version logged per campaign arm, and
     *    leaves a genuine wedge on the grid to fail the fixture loudly rather
     *    than be rescued quietly. */
    if ((sUnstickLevel >= 0 && levelId != sUnstickLevel) ||
        mdkr_adv_slot_for(levelId) >= 0 || racer->courseCheckpoint <= 0) {
        state->anchorX = obj->trans.x_position;
        state->anchorZ = obj->trans.z_position;
        state->immobile = 0;
        return;
    }

    /* The countdown, a finished racer, an airborne kart and a kart whose input
     * the game has taken away (results screen, dialogue, cutscene) are all
     * legitimate reasons to be going nowhere -- and in every one of them the
     * game's own recovery is still free to run, so there is no deadlock to
     * break. Measured: without the gRacerInputBlocked guard the hook fires
     * harmlessly but pointlessly 38 more times while the kart is parked on the
     * post-race results screen. */
    if (gRaceStartTimer != 0 || racer->raceFinished || !racer->groundedWheels ||
        gRacerInputBlocked) {
        state->anchorX = obj->trans.x_position;
        state->anchorZ = obj->trans.z_position;
        state->immobile = 0;
        return;
    }

    dx = obj->trans.x_position - state->anchorX;
    dz = obj->trans.z_position - state->anchorZ;
    if (((dx * dx) + (dz * dz)) >= (MDKR_UNSTICK_EPSILON * MDKR_UNSTICK_EPSILON)) {
        state->anchorX = obj->trans.x_position;
        state->anchorZ = obj->trans.z_position;
        state->immobile = 0;
        return;
    }

    state->immobile += updateRate;
    if (state->immobile < MDKR_UNSTICK_FRAMES) {
        return;
    }
    state->immobile = 0;
    if (racer->unk215 != 0) {
        if (mdkr_trace_enabled()) {
            mdkr_trace("autopilotunstick: player=%d level=%d wedged at (%.1f, %.1f, %.1f) checkpoint=%d "
                       "cooldown=%d -> 0 @frame~%d",
                       (int) playerIndex + 1, (int) levelId,
                       obj->trans.x_position, obj->trans.y_position,
                       obj->trans.z_position, (int) racer->courseCheckpoint,
                       (int) racer->unk215, g_frameCounter);
        }
        racer->unk215 = 0;
    }
}

/* ------------------------------------------- trophy rankings test boundary */

#define MDKR_TROPHY_MAX_ROUNDS 4
#define MDKR_TROPHY_MAX_RACERS 8

typedef struct MdkrTrophyOrder {
    s32 count;
    s8 racer[MDKR_TROPHY_MAX_RACERS];
} MdkrTrophyOrder;

static s32 sTrophyOrderInited;
static s32 sTrophyOrderCount;
static MdkrTrophyOrder sTrophyOrders[MDKR_TROPHY_MAX_ROUNDS];

static void mdkr_trophy_order_init(void) {
    const char *value;
    const char *cursor;

    if (sTrophyOrderInited) {
        return;
    }
    sTrophyOrderInited = TRUE;
    value = getenv("MDKR_TROPHY_ORDER");
    if (value == NULL || value[0] == '\0') {
        return;
    }
    cursor = value;
    while (*cursor != '\0' && sTrophyOrderCount < MDKR_TROPHY_MAX_ROUNDS) {
        MdkrTrophyOrder *order = &sTrophyOrders[sTrophyOrderCount];
        while (*cursor == ' ' || *cursor == '/') {
            cursor++;
        }
        while (*cursor != '\0' && *cursor != '/' &&
               order->count < MDKR_TROPHY_MAX_RACERS) {
            char *end;
            long racerId = strtol(cursor, &end, 10);
            if (end == cursor || racerId < 0 ||
                racerId >= MDKR_TROPHY_MAX_RACERS) {
                order->count = 0;
                return;
            }
            order->racer[order->count++] = (s8) racerId;
            cursor = end;
            while (*cursor == ' ' || *cursor == ',') {
                cursor++;
            }
        }
        sTrophyOrderCount++;
        if (*cursor == '/') {
            cursor++;
        }
    }
}

/**
 * Determinise one already-finished trophy race at the rankings boundary.
 *
 * The input is racer IDs in finish order; the production Settings representation
 * is the inverse mapping, starting_position[racerId] = place. Both the natural
 * order and requested order must be complete permutations, so this hook cannot
 * repair or hide a broken production finish.
 */
void mdkr_trophy_control_order(Settings *settings, s32 round, s32 racerCount) {
    MdkrTrophyOrder *order;
    s32 seenNatural = 0;
    s32 seenRequested = 0;
    s32 natural[MDKR_TROPHY_MAX_RACERS] = {
        -1, -1, -1, -1, -1, -1, -1, -1
    };
    s32 i;

    mdkr_trophy_order_init();
    if (settings == NULL || round < 0 || round >= sTrophyOrderCount ||
        racerCount <= 0 || racerCount > MDKR_TROPHY_MAX_RACERS) {
        return;
    }
    order = &sTrophyOrders[round];
    if (order->count != racerCount) {
        if (mdkr_trace_enabled()) {
            mdkr_trace("trophyorder: REJECT round=%d count=%d expected=%d",
                       (int) round, (int) order->count, (int) racerCount);
        }
        return;
    }
    for (i = 0; i < racerCount; i++) {
        s32 place = settings->racers[i].starting_position;
        s32 racerId = order->racer[i];
        if (place < 0 || place >= racerCount ||
            (seenNatural & (1 << place)) != 0 ||
            (seenRequested & (1 << racerId)) != 0) {
            if (mdkr_trace_enabled()) {
                mdkr_trace("trophyorder: REJECT round=%d invalid permutations",
                           (int) round);
            }
            return;
        }
        seenNatural |= 1 << place;
        seenRequested |= 1 << racerId;
        natural[place] = i;
    }
    if (seenNatural != ((1 << racerCount) - 1) ||
        seenRequested != ((1 << racerCount) - 1)) {
        return;
    }
    for (i = 0; i < racerCount; i++) {
        settings->racers[order->racer[i]].starting_position = i;
    }
    if (mdkr_trace_enabled()) {
        mdkr_trace("trophyorder: round=%d natural=%d,%d,%d,%d,%d,%d,%d,%d "
                   "controlled=%d,%d,%d,%d,%d,%d,%d,%d @frame~%d",
                   (int) round,
                   natural[0], natural[1], natural[2], natural[3],
                   natural[4], natural[5], natural[6], natural[7],
                   order->racer[0], order->racer[1], order->racer[2],
                   order->racer[3], order->racer[4], order->racer[5],
                   order->racer[6], order->racer[7], g_frameCounter);
    }
}

/**
 * Select the production rankings QUIT row once. Confirmation still comes from
 * the normal controller A edge, and menu_trophy_race_rankings_loop owns the
 * resulting transition. This exists because scripted stick repeat timing is a
 * poor proxy for the branch under test.
 */
void mdkr_trophy_control_menu_option(s32 completedRound, s32 *menuOption) {
    static s32 initialized;
    static s32 quitRound = -1;
    static s32 consumed;

    if (!initialized) {
        const char *value = getenv("MDKR_TROPHY_QUIT_AFTER");
        initialized = TRUE;
        if (value != NULL && value[0] != '\0') {
            quitRound = atoi(value);
        }
    }
    if (!consumed && menuOption != NULL && completedRound == quitRound) {
        *menuOption = 1;
        consumed = TRUE;
        if (mdkr_trace_enabled()) {
            mdkr_trace("trophyoption: selected QUIT afterRound=%d @frame~%d",
                       (int) completedRound, g_frameCounter);
        }
    }
}

void mdkr_trophy_control_world(Settings *settings) {
    const char *value = getenv("MDKR_TROPHY_WORLD");
    s32 world;
    if (settings == NULL || value == NULL || value[0] == '\0') {
        return;
    }
    world = atoi(value);
    if (world >= 1 && world <= 4) {
        settings->worldId = world;
    }
}

void mdkr_trophy_control_collision(Object *cabinet, Object *player) {
    static s32 initialized;
    static s32 enabled;
    static s32 consumed;
    if (!initialized) {
        const char *value = getenv("MDKR_TROPHY_COLLIDE");
        initialized = TRUE;
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (enabled && !consumed && cabinet != NULL && player != NULL &&
        cabinet->collisionData != NULL &&
        cabinet->collisionData->collidedObj == NULL) {
        cabinet->collisionData->collidedObj = player;
        consumed = TRUE;
        if (mdkr_trace_enabled()) {
            mdkr_trace("trophycabinet: controlled collision @frame~%d",
                       g_frameCounter);
        }
    }
}

/**
 * Bound long or locally non-convergent AI drives in the trophy integration
 * fixture without bypassing race_check_finish().
 *
 * This is intentionally a lap-state control, not a result control. Once the
 * threshold is reached every unfinished racer satisfies the production finish
 * predicate on the same call; race_check_finish() then assigns the complete
 * finish permutation itself. MDKR_TROPHY_ORDER may determinise that permutation
 * later, at rankings init, but production still computes all championship state.
 */
void mdkr_trophy_complete_race(Object **racers, s32 racerCount,
                               s32 requiredLaps) {
    static s32 initialized;
    static s32 threshold;
    static s32 activeLevel = -1;
    static s32 frames;
    static s32 reported;
    Settings *settings;
    s32 i;

    if (!initialized) {
        const char *value = getenv("MDKR_TROPHY_COMPLETE_AFTER");
        initialized = TRUE;
        threshold = (value != NULL) ? atoi(value) : 0;
        if (threshold < 1) {
            threshold = 0;
        }
    }
    if (threshold == 0 || racers == NULL || racerCount <= 0 ||
        requiredLaps <= 0 || get_trophy_race_world_id() == 0) {
        return;
    }
    settings = get_settings();
    if (settings == NULL) {
        return;
    }
    if (settings->courseId != activeLevel) {
        activeLevel = settings->courseId;
        frames = 0;
        reported = FALSE;
    }
    frames++;
    if (frames < threshold) {
        return;
    }
    for (i = 0; i < racerCount; i++) {
        if (racers[i] != NULL && racers[i]->racer != NULL &&
            !racers[i]->racer->raceFinished) {
            racers[i]->racer->lap = requiredLaps;
        }
    }
    if (!reported && mdkr_trace_enabled()) {
        reported = TRUE;
        mdkr_trace("trophycomplete: world=%d round=%d track=%d racers=%d "
                   "requiredLaps=%d after=%d @frame~%d",
                   (int) get_trophy_race_world_id(),
                   (int) gTrophyRaceRound, (int) settings->courseId,
                   (int) racerCount, (int) requiredLaps, (int) frames,
                   g_frameCounter);
    }
}

/* ------------------------------------------------- boss progress observation */
/*
 * Two hooks for the ONE piece of state the boss-race verdict turns on.
 *
 * racer_boss_finish() (game/src/vehicle_tricky.c) decides between "play the win
 * cutscene and award the amulet" and "just transition back" by testing
 * `settings->courseFlagsPtr[courseId] & RACE_CLEARED` -- and RACE_CLEARED on a
 * BOSS course is written in exactly one place, that same function's own win
 * branch.  So a first boss win presents correctly if and only if nothing has set
 * that bit (or the matching `settings->bosses` bit) beforehand.  Both bits are
 * written by index from several unrelated subsystems (golden balloons, doors,
 * triggers and the save unpacker all do `courseFlagsPtr[<some index>] |= ...`),
 * so "nothing sets it early" is a property that has to be MEASURED, not read:
 * one stray index and every first boss win loses its cutscene, with no crash and
 * nothing in the log.
 *
 *   MDKR_WATCH_COURSEFLAGS=<levelId>
 *       One "[BOSSW]" line per observed CHANGE to courseFlagsPtr[levelId] or to
 *       settings->bosses, with the frame and the level DKR currently considers
 *       current.  Polled at the frame boundary rather than hooked into each
 *       writer, deliberately: it therefore catches a write through a WRONG index
 *       just as well as a write through the intended one, which is the whole
 *       point.  Low bits are the race flags (1 visited, 2 cleared, 4 silver
 *       coins); bits 16+ are per-level balloon/door/trigger flags.
 *
 *   MDKR_BOSS_PRECLEARED=<levelId>
 *       Hold that boss course at "already beaten" -- courseFlagsPtr[levelId] |=
 *       RACE_VISITED|RACE_CLEARED and bosses |= 1 << world(levelId) -- which is
 *       the state a save carries once the boss has been beaten once.  This is
 *       the POSITIVE CONTROL for the above: it reproduces, from an unmodified
 *       binary, the reported "won the boss, got no cutscene and no amulet, and
 *       was dropped back in front of the boss door".  Re-applied every frame
 *       because FILE SELECT reallocates Settings and clear_game_progress() would
 *       otherwise wipe it.
 *
 *   MDKR_BOSS_WIN=1
 *       Award the HUMAN first place the instant its race finishes, by writing
 *       the one field racer_boss_finish() branches on -- racer->finishPosition.
 *
 *       This replaces MDKR_BOSS_SLOW=1 (platform/stubs_dkr.c, `velocity *= 0.15f`
 *       on the boss) in every check that needs the win branch, and the reason is
 *       the "closedloop" wave's central lesson.  Crippling the boss does not just
 *       change the boss: the human is driven by DKR's own AI, which paths
 *       relative to the field, so a crawling boss changes the HUMAN's racing line
 *       too.  Measured on Fire Mountain (boss 38) once the ROM's RNG seed, arctan
 *       table and table-based trig became the defaults: with MDKR_BOSS_SLOW the
 *       human climbs to y=4649 at frame 8700, clips the summit ledge, hangs with
 *       all four wheels off the ground (`[GRND] gw=0`, pitch ramping 3317 ->
 *       4133) for 84 frames and never crosses the line -- racer_boss_finish() is
 *       never reached at all, at any budget, so the whole win assertion goes
 *       vacuous.  The same route WITHOUT the slowdown finishes reliably in every
 *       arm measured (y=4868, cp=36, fin=1).
 *
 *       So: leave the physics alone, let the race finish the way it always does,
 *       and set the position.  What the checks assert is "finishPos == 1 implies
 *       the WIN cutscene, the amulet and RACE_CLEARED" -- an implication about the
 *       verdict, not about who is faster -- and this isolates exactly that one
 *       variable while leaving the racing line bit-identical to the losing arm.
 *
 *       Written from the frame boundary rather than at the finish itself so this
 *       stays out of game/ race-finish code.  There is a wide window: measured on
 *       boss 38, raceFinished latches at frame ~8900 and racer_boss_finish() does
 *       not read finishPosition until ~9095.
 *
 * All three are no-ops unless set.  Called once per frame from
 * platform_frame_sync().
 */
static s32 sBossWatchInited = 0;
static s32 sBossWatchLevel = -1;
static s32 sBossPreclearLevel = -1;
static s32 sBossPreclearDone = 0;
static s32 sBossWatchValid = 0;
static u32 sBossWatchFlags = 0;
static u32 sBossWatchBosses = 0;
static s32 sBossForceWin = 0;
static s32 sBossForceWinDone = 0;

/**
 * MDKR_BOSS_WIN=1 -- give the human first place as soon as its race finishes.
 *
 * Walks the object list for the racer whose playerIndex is not PLAYER_COMPUTER
 * (there is exactly one on a boss route) and, once DKR has latched
 * racer->raceFinished, writes finishPosition = 1. See the block comment at the
 * top of this file for why this replaced MDKR_BOSS_SLOW: the velocity hack moved
 * the HUMAN's AI line as well as the boss's and drove it off the Fire Mountain
 * summit, which made the win branch unreachable rather than merely slower.
 *
 * Applied every frame while it holds, not once: nothing else writes
 * finishPosition after the finish, but the cost is one integer compare and this
 * must not depend on catching a single frame.
 */
static void mdkr_boss_force_win(void) {
    s32 i;
    for (i = 0; i < gObjectCount; i++) {
        Object *obj = gObjPtrList[i];
        Object_Racer *racer;
        if (obj == NULL || obj->behaviorId != BHV_RACER || obj->racer == NULL) {
            continue;
        }
        racer = obj->racer;
        if (racer->playerIndex == PLAYER_COMPUTER || !racer->raceFinished) {
            continue;
        }
        if (racer->finishPosition != 1) {
            if (!sBossForceWinDone && mdkr_trace_enabled()) {
                sBossForceWinDone = 1;
                mdkr_trace("bossforcewin: finishPosition %d -> 1 (playerIndex=%d raceFinished=%d) @frame~%d",
                           (int) racer->finishPosition, (int) racer->playerIndex,
                           (int) racer->raceFinished, g_frameCounter);
            }
            racer->finishPosition = 1;
        }
    }
}

void mdkr_boss_state_probe(void) {
    Settings *settings;
    s32 levelCount;
    s32 worldCount;

    if (!sBossWatchInited) {
        const char *e;
        sBossWatchInited = 1;
        e = getenv("MDKR_WATCH_COURSEFLAGS");
        sBossWatchLevel = (e != NULL && e[0] != '\0') ? atoi(e) : -1;
        e = getenv("MDKR_BOSS_PRECLEARED");
        sBossPreclearLevel = (e != NULL && e[0] != '\0') ? atoi(e) : -1;
        e = getenv("MDKR_BOSS_WIN");
        sBossForceWin = (e != NULL && e[0] != '\0' && e[0] != '0');
    }
    if (sBossForceWin) {
        mdkr_boss_force_win();
    }
    if (sBossWatchLevel < 0 && sBossPreclearLevel < 0) {
        return;
    }
    settings = get_settings();
    if (settings == NULL || settings->courseFlagsPtr == NULL) {
        return;
    }
    /* level_count() reads gNumberOfLevelHeaders; leveltable_world() silently
     * returns 0 before the level table is loaded, which would seed the wrong
     * boss bit, so nothing is done until the table is there. */
    level_count(&levelCount, &worldCount);
    if (levelCount <= 0) {
        return;
    }

    if (sBossPreclearLevel >= 0 && sBossPreclearLevel < levelCount) {
        /* leveltable_world() is s8 and returns -1 for a level with no world, so
         * the shift is guarded rather than trusted. */
        s32 world = (s32) leveltable_world(sBossPreclearLevel);
        settings->courseFlagsPtr[sBossPreclearLevel] |= RACE_VISITED | RACE_CLEARED;
        if (world >= 0 && world < 16) {
            settings->bosses |= (u16) (1 << world);
        }
        if (!sBossPreclearDone) {
            sBossPreclearDone = 1;
            if (mdkr_trace_enabled()) {
                mdkr_trace("bosspreclear: level=%d world=%d courseFlags=0x%x bosses=0x%x @frame~%d",
                           (int) sBossPreclearLevel, (int) world,
                           (unsigned) settings->courseFlagsPtr[sBossPreclearLevel], (unsigned) settings->bosses,
                           g_frameCounter);
            }
        }
    }

    if (sBossWatchLevel >= 0 && sBossWatchLevel < levelCount && mdkr_trace_enabled()) {
        u32 flags = (u32) settings->courseFlagsPtr[sBossWatchLevel];
        u32 bosses = (u32) settings->bosses;
        if (!sBossWatchValid || flags != sBossWatchFlags || bosses != sBossWatchBosses) {
            mdkr_trace("[BOSSW] frame=%d courseFlags[%d]=0x%x (was 0x%x) bosses=0x%x (was 0x%x) "
                       "courseId=%d worldId=%d",
                       g_frameCounter, (int) sBossWatchLevel, (unsigned) flags, (unsigned) sBossWatchFlags,
                       (unsigned) bosses, (unsigned) sBossWatchBosses, (int) settings->courseId,
                       (int) settings->worldId);
            sBossWatchFlags = flags;
            sBossWatchBosses = bosses;
            sBossWatchValid = 1;
        }
    }
}

#endif /* NATIVE_PORT */
