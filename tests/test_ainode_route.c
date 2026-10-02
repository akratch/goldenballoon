/*
 * Challenge-arena AI route search (func_8001CD28) against the original game.
 *
 * White-box by design, like test_void_pairs.c: this target links the
 * PRODUCTION game/src/objects.c (dead-stripped to the AI-node closure) so the
 * search under test is the shipped one, not a copy. retail_route_search()
 * below is the v1.7.0 body, which is the decomp's N64 body, copied verbatim.
 * The only lines added to it are marked TEST BUDGET: a retail search whose
 * target is absent from a cyclic graph never empties its frontier (the N64
 * spins until its s32 cost wraps), so the reference stops at the same pop
 * budget, with the same fallback, as the native search. Everything else,
 * including which nodes are re-added after they were expanded, is retail.
 *
 * Every graph and query must give the same next node AND the same direction
 * write on the start node. The queries cover what the arena AI asks: a node
 * target (case 4, `id | 0x100`), including the racer's own node; a tag
 * target (cases 3/5/7); a target behind the racer (arg2 = the node it came
 * from); and targets that are absent. The reference's own termination reason
 * is counted, so the run proves it compared searches that found their target,
 * searches that emptied their frontier and searches that hit the budget.
 *
 * The second half pins the native rebuild guards of ainode_update and the
 * search's stale-link checks by behaviour (they used to be source-text pins
 * in check_runtime_safety.py): each case fails, or trips ASan, when its guard
 * is removed.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "objects.h"
#include "runtime_contracts.h"

#define NODES MDKR_AI_NODE_COUNT

/* File-scope state of game/src/objects.c used by the AI-node functions. */
extern Object *(*gAINodes)[NODES];
extern Object **gObjPtrList;
extern s32 gObjectCount;
extern s32 gInitAINodes;

/* Link stubs for the trace plumbing on ainode_update's duplicate-ID path.
 * Everything else objects.c references is dead-stripped. */
int mdkr_trace_enabled(void) {
    return 0;
}

void mdkr_trace(const char *fmt, ...) {
    (void) fmt;
}

static int failures;

#define EXPECT(cond, ...)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            if (failures < 40) {                                            \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
                fprintf(stderr, __VA_ARGS__);                               \
                fputc('\n', stderr);                                        \
            }                                                               \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ---------------------------------------------------------------------- */
/* Reference: v1.7.0 game/src/objects.c func_8001CD28, verbatim.          */
/* ---------------------------------------------------------------------- */

enum { STOP_NONE, STOP_TARGET, STOP_DRAINED, STOP_BUDGET };
static int sRetailStop;

#define AINODE_COUNT NODES

static s16 retail_route_search(s32 arg0, s32 arg1, s32 arg2, s32 arg3) {
    s16 result;
    s32 sp370;
    s16 var_t1;
    s16 sp36C;
    s16 temp2;
    s16 i;
    LevelObjectEntry_AiNode *aiNodeEntry;
    s8 someBool;
    s8 var_s3;
    s8 var_s5;
    s8 temp;
    s32 var_ra;
    Object_AiNode *aiNode;
    Object *aiNodeObj;
    s32 sp154[AINODE_COUNT];
    s8 spD4[AINODE_COUNT];
    s8 sp54[AINODE_COUNT];
    s32 pops = 0;          /* TEST BUDGET */
    sRetailStop = STOP_NONE; /* TEST BUDGET */

    // Only matches with do {} while?
    i = 0;
    do {
        sp154[i] = 0;
        i++;
    } while (i < AINODE_COUNT);

    aiNodeObj = (*gAINodes)[arg0];
    aiNode = aiNodeObj->ai_node;
    aiNodeEntry = &aiNodeObj->level_entry->aiNode;
    var_t1 = 0;
    var_ra = 0;
    result = NODE_NONE;
    var_s3 = 0;
    someBool = 1;
    var_s5 = 0;
    do {
        for (i = 0; i < 4; i++) {
            if (aiNode->nodeObj[i] != 0) {
                temp2 = aiNodeEntry->adjacent[i];
                temp = 1;
                if (((arg0 == aiNodeEntry->nodeID) && (temp2 == arg2)) ||
                    ((arg2 == aiNodeEntry->nodeID) && (temp2 == arg0))) {
                    temp = 0;
                    if (var_s5 != 0) {
                        temp = 1;
                    }
                    var_s5++;
                }
                if (temp != 0) {
                    sp36C = 0;
                    temp = 0;
                    while ((sp36C < var_t1) && (temp2 != spD4[sp36C])) {
                        sp36C++;
                    }

                    sp370 = aiNode->distToNode[i] + var_ra;
                    if (sp36C == var_t1) {
                        temp = 2;
                    } else if (sp370 < sp154[temp2]) {
                        temp = 1;
                    }
                }
                if (temp != 0) {
                    while (sp36C < var_t1 - 1) {
                        spD4[sp36C] = spD4[sp36C + 1];
                        sp54[sp36C] = sp54[sp36C + 1];
                        sp36C++;
                    }
                    if (temp == 2) {
                        var_t1++;
                    }
                    sp154[temp2] = sp370;

                    sp36C = var_t1 - 1;

                    sp54[sp36C] = (someBool) ? aiNodeEntry->adjacent[i] : var_s3;
                    spD4[sp36C] = aiNodeEntry->adjacent[i];

                    while ((sp36C > 0) && (sp154[spD4[sp36C - 1]] < sp154[spD4[sp36C]])) {
                        temp = spD4[sp36C];
                        spD4[sp36C] = spD4[sp36C - 1];
                        spD4[sp36C - 1] = temp;
                        temp = sp54[sp36C];
                        sp54[sp36C] = sp54[sp36C - 1] & 0xFF & 0xFF & 0xFF;
                        sp54[sp36C - 1] = temp & 0xFF;
                        sp36C--;
                    }
                }
            }
        }

        if (var_t1 > 0) {
            var_t1--;
            aiNodeObj = (*gAINodes)[spD4[var_t1]];
            aiNodeEntry = &aiNodeObj->level_entry->aiNode;
            var_s3 = sp54[var_t1];
            var_ra = sp154[aiNodeEntry->nodeID];
            aiNode = aiNodeObj->ai_node;
            someBool = 0;
            if (arg1 & 0x100) {
                if ((arg1 & 0x7F) == spD4[var_t1]) {
                    result = var_s3;
                }
            } else if (arg1 == aiNodeEntry->unk8) {
                result = var_s3;
            }
            if (result != 0xFF) sRetailStop = STOP_TARGET; /* TEST BUDGET */
            if ((var_t1 == 0) && (result == 0xFF)) {
                result = var_s3;
                sRetailStop = STOP_DRAINED; /* TEST BUDGET */
            }
            /* TEST BUDGET: stop where the native search stops, the same way. */
            if (++pops >= MDKR_AI_ROUTE_POP_BUDGET && result == 0xFF) {
                result = var_s3;
                sRetailStop = STOP_BUDGET;
            }
        }
    } while ((result == NODE_NONE) && (var_t1 > 0));

    if (result != 0xFF) {
        aiNodeObj = (*gAINodes)[arg0];
        aiNodeEntry = &aiNodeObj->level_entry->aiNode;
        aiNode = aiNodeObj->ai_node;
        for (i = 0; (i < 4) && (result != aiNodeEntry->adjacent[i]); i++) {}
        if (i < 4) {
            aiNode->directions[arg3] = i;
        }
    }
    return result;
}

#undef AINODE_COUNT

/* ---------------------------------------------------------------------- */
/* Graph fixture                                                          */
/* ---------------------------------------------------------------------- */

typedef struct Graph {
    Object objects[NODES + 2];
    Object_AiNode state[NODES + 2];
    LevelObjectEntry entries[NODES + 2];
    Object *table[NODES];
    Object *list[NODES + 2];
    int count;
} Graph;

static Graph sGraph;

static u32 sRng = 0x2545F491u;

static u32 rng(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static u32 rng_below(u32 bound) {
    return bound ? rng() % bound : 0;
}

/* Fresh nodes 0..count-1, no links, all tags 0, placed on a line. */
static void graph_reset(int count) {
    memset(&sGraph, 0, sizeof sGraph);
    sGraph.count = count;
    for (int i = 0; i < count; ++i) {
        Object *obj = &sGraph.objects[i];
        obj->behaviorId = BHV_AINODE;
        obj->level_entry = &sGraph.entries[i];
        obj->ai_node = &sGraph.state[i];
        obj->trans.x_position = (f32) (i * 100);
        sGraph.entries[i].aiNode.nodeID = (u8) i;
        memset(sGraph.entries[i].aiNode.adjacent, NODE_NONE, 4);
        sGraph.list[i] = obj;
        sGraph.table[i] = obj;
    }
    gAINodes = &sGraph.table;
    gObjPtrList = sGraph.list;
    gObjectCount = count;
}

/* Build the links with the production rebuild, from positions. */
static void graph_rebuild(void) {
    gInitAINodes = TRUE;
    ainode_update();
}

/* Set one link directly with an explicit cost, the way ainode_update would
 * leave it, for graphs whose costs do not come from positions. */
static void graph_link(int from, int slot, int to, s32 cost) {
    sGraph.entries[from].aiNode.adjacent[slot] = (u8) to;
    sGraph.state[from].nodeObj[slot] = &sGraph.objects[to];
    sGraph.state[from].distToNode[slot] = cost;
}

static void snapshot_directions(s8 out[NODES + 2][4]) {
    for (int i = 0; i < NODES + 2; ++i) memcpy(out[i], sGraph.state[i].directions, 4);
}

static void restore_directions(s8 in[NODES + 2][4]) {
    for (int i = 0; i < NODES + 2; ++i) memcpy(sGraph.state[i].directions, in[i], 4);
}

static long sStops[4];
static long sCompared;

/* Run both searches from the same state; require the same answer and the
 * same direction writes. Returns the shared result. */
static s16 compare(s32 start, s32 target, s32 previous, s32 lane, const char *label) {
    s8 before[NODES + 2][4], reference[NODES + 2][4], native[NODES + 2][4];
    snapshot_directions(before);
    s16 expected = retail_route_search(start, target, previous, lane);
    int stop = sRetailStop;
    snapshot_directions(reference);
    restore_directions(before);
    const u32 budget_hits = mdkr_ai_route_budget_hits();
    s16 actual = func_8001CD28(start, target, previous, lane);
    snapshot_directions(native);
    sStops[stop]++;
    sCompared++;
    /* The budget announces itself ([AI_ROUTE_BUDGET], counted here) exactly
     * when it ends a search, so a gate can require that no retail route ever
     * reaches it. */
    EXPECT(mdkr_ai_route_budget_hits() - budget_hits == (u32) (stop == STOP_BUDGET),
           "%s: start=%d target=0x%x: budget reported %u time(s), original stop %d",
           label, (int) start, (unsigned) target,
           (unsigned) (mdkr_ai_route_budget_hits() - budget_hits), stop);
    EXPECT(actual == expected,
           "%s: start=%d target=0x%x previous=%d lane=%d: native %d, original %d (stop %d)",
           label, (int) start, (unsigned) target, (int) previous, (int) lane, actual, expected, stop);
    EXPECT(!memcmp(reference, native, sizeof native),
           "%s: start=%d target=0x%x previous=%d: direction writes differ", label, (int) start,
           (unsigned) target, (int) previous);
    return expected;
}

/* ---------------------------------------------------------------------- */
/* Named cases (audit A3 #1)                                              */
/* ---------------------------------------------------------------------- */

/* The in-binary self-test's graph: nodes at x = 0, 10, 20, each linked to
 * the other two. With the target on the start node the original search
 * re-adds node 0 while expanding node 1 and returns 1; a search that never
 * revisits an expanded node cannot match it and returns 2. */
static void case_triangle(void) {
    graph_reset(3);
    for (int i = 0; i < 3; ++i) {
        sGraph.objects[i].trans.x_position = (f32) (10 * i);
        sGraph.entries[i].aiNode.adjacent[0] = (u8) ((i + 1) % 3);
        sGraph.entries[i].aiNode.adjacent[1] = (u8) ((i + 2) % 3);
    }
    graph_rebuild();
    EXPECT(compare(0, 0x100, NODE_NONE, 0, "triangle target=start") == 1,
           "triangle: target on the start node must route through node 1");
    EXPECT(compare(0, 0x102, NODE_NONE, 0, "triangle target=2") == 2, "triangle: direct target");
    EXPECT(compare(0, 0x101, NODE_NONE, 0, "triangle target=1") == 1, "triangle: direct target");
    for (s32 target = 0x100; target < 0x103; ++target)
        for (s32 previous = 0; previous < 3; ++previous)
            compare(previous == 0 ? 1 : 0, target, previous, 1, "triangle previous");
    compare(0, 0x17F, NODE_NONE, 0, "triangle absent node");
    compare(0, 99, NODE_NONE, 0, "triangle absent tag");
}

/* A ring 0-1-2-3-4-5-0 with the racer at 0, having come from 5. The target
 * 5 is behind it: the original search skips the 0->5 reverse edge once, then
 * reaches 5 by expanding the re-added start node. */
static void case_ring_behind(void) {
    graph_reset(6);
    for (int i = 0; i < 6; ++i) {
        sGraph.objects[i].trans.x_position = (f32) (i < 3 ? i * 100 : (6 - i) * 100);
        sGraph.objects[i].trans.z_position = (f32) (i < 3 ? 0 : 150);
        sGraph.entries[i].aiNode.adjacent[0] = (u8) ((i + 1) % 6);
        sGraph.entries[i].aiNode.adjacent[1] = (u8) ((i + 5) % 6);
    }
    graph_rebuild();
    for (s32 target = 0x100; target < 0x106; ++target)
        for (s32 lane = 0; lane < 4; ++lane) compare(0, target, 5, lane, "ring behind");
    for (int start = 0; start < 6; ++start)
        for (s32 target = 0x100; target < 0x106; ++target) {
            compare(start, target, (start + 5) % 6, 0, "ring from behind");
            compare(start, target, (start + 1) % 6, 0, "ring reversed");
            compare(start, target, NODE_NONE, 0, "ring fresh");
        }
}

/* The tag on the racer's own node (cases 3/5/7 when the current node
 * carries the searched tag), and tags present once, twice or never. */
static void case_tags(void) {
    graph_reset(5);
    for (int i = 0; i < 5; ++i) {
        sGraph.objects[i].trans.x_position = (f32) (i * 90);
        sGraph.objects[i].trans.z_position = (f32) ((i & 1) * 70);
        sGraph.entries[i].aiNode.adjacent[0] = (u8) ((i + 1) % 5);
        sGraph.entries[i].aiNode.adjacent[1] = (u8) ((i + 4) % 5);
        if (i == 0) sGraph.entries[i].aiNode.adjacent[2] = 2;
        if (i == 2) sGraph.entries[i].aiNode.adjacent[2] = 0;
    }
    sGraph.entries[0].aiNode.unk8 = 1;
    sGraph.entries[3].aiNode.unk8 = 1;
    sGraph.entries[4].aiNode.unk8 = 5;
    graph_rebuild();
    for (int start = 0; start < 5; ++start)
        for (s32 previous = -1; previous < 5; ++previous) {
            compare(start, 1, previous < 0 ? NODE_NONE : previous, 2, "tag 1");
            compare(start, 5, previous < 0 ? NODE_NONE : previous, 2, "tag 5");
            compare(start, 6, previous < 0 ? NODE_NONE : previous, 2, "absent tag 6");
        }
}

/* A dead-end chain: expanding the far end empties the frontier, so an
 * absent target ends on the last popped node's first hop. */
static void case_chain(void) {
    graph_reset(4);
    for (int i = 0; i < 4; ++i) {
        if (i + 1 < 4) sGraph.entries[i].aiNode.adjacent[0] = (u8) (i + 1);
    }
    graph_rebuild();
    EXPECT(compare(0, 0x17F, NODE_NONE, 0, "chain absent") == 1, "chain: drained fallback");
    compare(0, 0x103, NODE_NONE, 0, "chain far end");
    compare(3, 0x100, NODE_NONE, 0, "chain no exits");
}

/* ---------------------------------------------------------------------- */
/* Random graphs                                                          */
/* ---------------------------------------------------------------------- */

static void random_graph(int count, int from_positions) {
    graph_reset(count);
    for (int i = 0; i < count; ++i) {
        sGraph.objects[i].trans.x_position = (f32) ((s32) rng_below(8000) - 4000);
        sGraph.objects[i].trans.y_position = (f32) ((s32) rng_below(600) - 300);
        sGraph.objects[i].trans.z_position = (f32) ((s32) rng_below(8000) - 4000);
        sGraph.entries[i].aiNode.unk8 = (u8) (rng_below(4) == 0 ? 1 + rng_below(7) : 0);
    }
    for (int i = 0; i < count; ++i) {
        /* A ring keeps most graphs cyclic, like every retail arena; the
         * other slots add chords, back links, gaps and the odd self link. */
        int slots = 1 + (int) rng_below(4);
        for (int s = 0; s < slots; ++s) {
            int to;
            if (s == 0) to = (i + 1) % count;
            else if (rng_below(8) == 0) to = NODE_NONE;
            else if (rng_below(16) == 0) to = i;
            else to = (int) rng_below((u32) count);
            if (from_positions) {
                sGraph.entries[i].aiNode.adjacent[s] = (u8) to;
            } else if (to != NODE_NONE) {
                graph_link(i, s, to, (s32) rng_below(rng_below(10) == 0 ? 3 : 2500));
            }
        }
    }
    if (from_positions) graph_rebuild();
}

static void case_random(void) {
    for (int g = 0; g < 360; ++g) {
        int count = 2 + (int) rng_below(g < 180 ? 12 : 40);
        random_graph(count, g & 1);
        for (int q = 0; q < 48; ++q) {
            s32 start = (s32) rng_below((u32) count);
            s32 target;
            u32 kind = rng_below(16);
            if (kind < 9) target = 0x100 | (s32) rng_below((u32) count);
            else if (kind < 11) target = 0x100 | start;
            else target = sGraph.entries[rng_below((u32) count)].aiNode.unk8; /* 0 = untagged */
            /* An absent target runs a cyclic search to the budget. One per
             * few graphs covers that regime and keeps the run quick. */
            if (q == 0 && (g & 3) == 0)
                target = (g & 4) ? 0x100 | (s32) (count + rng_below((u32) (NODES - count))) : 8;
            s32 previous;
            u32 p = rng_below(4);
            const LevelObjectEntry_AiNode *entry = &sGraph.entries[start].aiNode;
            if (p == 0) previous = NODE_NONE;
            else if (p == 1) previous = (s32) rng_below((u32) count);
            else previous = entry->adjacent[rng_below(4)];
            compare(start, target, previous, (s32) rng_below(4), "random");
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Native guards, by behaviour                                            */
/* ---------------------------------------------------------------------- */

/* ainode_update: an explicit ID that repeats must neither overflow the
 * 128-entry rebuild arrays nor replace the first object that claimed it
 * (129 objects share ID 0 here, then one object is ID 1). */
static void case_rebuild_duplicates(void) {
    graph_reset(2);
    for (int i = 0; i < NODES + 1; ++i) sGraph.list[i] = &sGraph.objects[0];
    Object duplicate = sGraph.objects[0];
    LevelObjectEntry duplicateEntry = sGraph.entries[0];
    duplicate.level_entry = &duplicateEntry;
    sGraph.list[NODES] = &duplicate;
    sGraph.list[NODES + 1] = &sGraph.objects[1];
    sGraph.entries[0].aiNode.adjacent[0] = 1;
    duplicateEntry.aiNode.adjacent[0] = NODE_NONE;
    gObjectCount = NODES + 2;
    graph_rebuild();
    EXPECT(sGraph.table[0] == &sGraph.objects[0], "duplicate ID: the first object must keep the slot");
    EXPECT(sGraph.table[1] == &sGraph.objects[1], "duplicate ID: later IDs must still register");
    EXPECT(sGraph.state[0].nodeObj[0] == &sGraph.objects[1], "duplicate ID: first object's link");
}

/* ainode_update: a rebuild owns every link. A slot whose replacement is out
 * of the node domain must lose the previous pointer and cost. */
static void case_rebuild_stale_link(void) {
    graph_reset(2);
    sGraph.entries[0].aiNode.adjacent[0] = 1;
    sGraph.objects[1].trans.x_position = 10;
    graph_rebuild();
    EXPECT(sGraph.state[0].nodeObj[0] == &sGraph.objects[1] && sGraph.state[0].distToNode[0] == 10,
           "rebuild: live link");
    sGraph.entries[0].aiNode.adjacent[0] = 200;
    graph_rebuild();
    EXPECT(sGraph.state[0].nodeObj[0] == NULL, "rebuild: out-of-domain link keeps a stale pointer");
    EXPECT(sGraph.state[0].distToNode[0] == 0, "rebuild: out-of-domain link keeps a stale cost");
    EXPECT(sGraph.entries[0].aiNode.adjacent[0] == NODE_NONE, "rebuild: out-of-domain ID not cleared");
}

/* ainode_update: link costs are the truncated distance, widened past s16,
 * and a nonfinite or over-budget distance drops the link instead of storing
 * a converted garbage cost. */
static void case_rebuild_costs(void) {
    graph_reset(3);
    sGraph.objects[0].trans.x_position = -32768;
    sGraph.objects[1].trans.x_position = 32767;
    sGraph.objects[2].trans.x_position = 1.0e20f;
    sGraph.entries[0].aiNode.adjacent[0] = 1;
    sGraph.entries[0].aiNode.adjacent[1] = 2;
    graph_rebuild();
    EXPECT(sGraph.state[0].distToNode[0] == 65535 && sGraph.state[0].nodeObj[0] == &sGraph.objects[1],
           "rebuild: full map-coordinate range must keep a positive cost");
    EXPECT(sGraph.state[0].nodeObj[1] == NULL && sGraph.entries[0].aiNode.adjacent[1] == NODE_NONE,
           "rebuild: an over-budget distance must drop the link");
    EXPECT(func_8001CD28(0, 0x101, NODE_NONE, 0) == 1, "wide cost: search still reaches node 1");
}

/* func_8001CD28: a link whose pointer/ID pair no longer agrees (links not
 * rebuilt since), or whose ID is outside the node domain, is skipped rather
 * than used to index the search's 128-entry scratch arrays. */
static void case_search_stale_links(void) {
    graph_reset(3);
    sGraph.entries[0].aiNode.adjacent[0] = 1;
    sGraph.entries[0].aiNode.adjacent[1] = 2;
    sGraph.entries[1].aiNode.adjacent[0] = 2;
    graph_rebuild();
    EXPECT(func_8001CD28(0, 0x102, NODE_NONE, 0) == 2, "stale: control search");
    sGraph.entries[0].aiNode.adjacent[0] = 255;
    sGraph.entries[0].aiNode.adjacent[1] = 255;
    EXPECT(func_8001CD28(0, 0x102, NODE_NONE, 0) == NODE_NONE, "stale: ID 255 with a live pointer");
    sGraph.entries[0].aiNode.adjacent[0] = 2; /* pointer still names node 1 */
    sGraph.entries[0].aiNode.adjacent[1] = NODE_NONE;
    EXPECT(func_8001CD28(0, 0x102, NODE_NONE, 0) == NODE_NONE, "stale: pointer and ID disagree");
    graph_rebuild();
    sGraph.state[0].distToNode[0] = -1;
    sGraph.state[0].distToNode[1] = -5;
    EXPECT(func_8001CD28(0, 0x102, NODE_NONE, 0) == NODE_NONE, "stale: negative cost");
    for (s32 root = -2; root <= NODES; ++root)
        if (root < 0 || root >= 3)
            EXPECT(func_8001CD28(root, 0x101, NODE_NONE, 0) == NODE_NONE, "invalid root %d", (int) root);
}

int main(void) {
    case_triangle();
    case_ring_behind();
    case_tags();
    case_chain();
    case_random();
    case_rebuild_duplicates();
    case_rebuild_stale_link();
    case_rebuild_costs();
    case_search_stale_links();

    /* The comparison is only evidence if it exercised every way the original
     * search ends: target found, frontier emptied and (for targets retail
     * could never reach) the budget. */
    EXPECT(sStops[STOP_TARGET] > 1000, "too few target-found searches: %ld", sStops[STOP_TARGET]);
    EXPECT(sStops[STOP_DRAINED] > 0, "no search emptied its frontier");
    EXPECT(sStops[STOP_BUDGET] > 0, "no search reached the budget");
    printf("ainode route: compared=%ld target=%ld drained=%ld budget=%ld empty=%ld\n", sCompared,
           sStops[STOP_TARGET], sStops[STOP_DRAINED], sStops[STOP_BUDGET], sStops[STOP_NONE]);
    if (failures) {
        fprintf(stderr, "ainode route: FAIL (%d)\n", failures);
        return 1;
    }
    puts("ainode route: PASS");
    return 0;
}
