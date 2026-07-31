/*
 * Clean-room libaudio event-queue implementation.
 *
 * Kept in its own translation unit so the exact production queue and recovery
 * behavior can be exercised by a ROM-free regression test.
 */

#include "audio_event_queue.h"

#include <stdlib.h>
#include <string.h>

#ifdef NATIVE_PORT
#include <stdio.h>
#endif

#ifdef TARGET_N64
/* The matching-target compatibility string.h intentionally exposes only the
 * retail-used subset, while this clean-room unit also needs
 * zero-initialization. */
extern void *memset(void *dest, int value, size_t count);
#endif

#ifdef NATIVE_PORT
#define PORT_AUDIO_EVTQ_TRACKED_MAX 16

typedef struct PortAudioEventQueueTrack_s {
    ALEventQueue *queue;
    u32 capacity;
    u32 occupancy;
    u32 high_water;
    u32 dropped_posts;
    u32 empty_polls;
    u32 heartbeat_recoveries;
} PortAudioEventQueueTrack;

static PortAudioEventQueueTrack
    s_event_queue_tracks[PORT_AUDIO_EVTQ_TRACKED_MAX];

static PortAudioEventQueueTrack *portAudioEventQueueFind(ALEventQueue *evtq,
                                                         s32 create) {
    PortAudioEventQueueTrack *free_slot = NULL;
    s32 i;

    if (evtq == NULL) {
        return NULL;
    }

    for (i = 0; i < PORT_AUDIO_EVTQ_TRACKED_MAX; i++) {
        PortAudioEventQueueTrack *track = &s_event_queue_tracks[i];

        if (track->queue == evtq) {
            return track;
        }
        if (free_slot == NULL && track->queue == NULL) {
            free_slot = track;
        }
    }

    if (create && free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->queue = evtq;
        return free_slot;
    }

    return NULL;
}

static void portAudioEventQueueTrackNew(ALEventQueue *evtq, s32 capacity) {
    PortAudioEventQueueTrack *track = portAudioEventQueueFind(evtq, TRUE);

    if (track != NULL) {
        memset(track, 0, sizeof(*track));
        track->queue = evtq;
        track->capacity = capacity > 0 ? (u32)capacity : 0;
    }
}

/*
 * mdkr64 event-queue instrumentation. These two counters predate the ported
 * queue and are kept under their original names; MDKR_EVTQ_STATS=1 reports
 * each new high-water mark. The richer per-queue tracking above is a superset
 * that feeds them.
 */
s32 alEvtqDropCount = 0;   /* total dropped posts, all queues */
s32 alEvtqPeakDepth = 0;   /* deepest queue seen              */

static int s_evtqStats = -1;

static void portAudioEventQueueStatsEnabled(void) {
    if (s_evtqStats < 0) {
        const char *e = getenv("MDKR_EVTQ_STATS");
        s_evtqStats = (e != NULL && e[0] == '1') ? 1 : 0;
    }
}

static void portAudioEventQueueTrackInsert(ALEventQueue *evtq) {
    PortAudioEventQueueTrack *track;

    if (evtq == NULL) {
        return;
    }

    track = portAudioEventQueueFind(evtq, FALSE);
    if (track == NULL) {
        return;
    }

    if (track->occupancy != ~(u32)0) {
        track->occupancy++;
    }
    if (track->occupancy > track->high_water) {
        track->high_water = track->occupancy;
        /* Per-queue high-water telemetry in the exact format the SGI engine
         * emitted and check_attract_demo.py parses ([EVTQ] qN(ptr) new peak
         * P of C, documented in tests/README.md). The clean-room swap deleted
         * the line with the code that printed it, and the attract gate's fixed
         * arm — which exists to prove the queue never exceeds capacity across a
         * full rolling-demo cycle — went blind. MDKR_TRACE-gated like every
         * route the gates drive.
         *
         * The parenthesised field is the queue ADDRESS, as it was before the
         * swap: check_browser_runtime.py keys its per-index identity assertion
         * ("event queue qN changed identity during one run") on this field, and
         * a constant literal there makes that assertion unfalsifiable. */
        {
            static int s_evtqTrace = -1;
            if (s_evtqTrace < 0) {
                const char *t = getenv("MDKR_TRACE");
                s_evtqTrace = (t != NULL && t[0] != '\0' && t[0] != '0');
            }
            if (s_evtqTrace) {
                printf("[EVTQ] q%d(%p) new peak %u of %u\n",
                       (int)(track - s_event_queue_tracks), (void *)evtq,
                       track->high_water, track->capacity);
            }
        }
        if ((s32)track->high_water > alEvtqPeakDepth) {
            alEvtqPeakDepth = (s32)track->high_water;
            portAudioEventQueueStatsEnabled();
            if (s_evtqStats == 1) {
                printf("[AUDIO] evtq %p new peak depth %d\n",
                       (void *)evtq, alEvtqPeakDepth);
            }
        }
    }
}

static void portAudioEventQueueTrackRemove(ALEventQueue *evtq) {
    PortAudioEventQueueTrack *track = portAudioEventQueueFind(evtq, FALSE);

    if (track != NULL && track->occupancy > 0) {
        track->occupancy--;
    }
}

static void portAudioEventQueueTrackDrop(ALEventQueue *evtq) {
    PortAudioEventQueueTrack *track = portAudioEventQueueFind(evtq, FALSE);

    alEvtqDropCount++;
    if (track != NULL) {
        if (track->dropped_posts != ~(u32)0) {
            track->dropped_posts++;
        }
    }
}

static void portAudioEventQueueTrackEmptyPoll(ALEventQueue *evtq) {
    PortAudioEventQueueTrack *track = portAudioEventQueueFind(evtq, FALSE);

    if (track != NULL) {
        if (track->empty_polls != ~(u32)0) {
            track->empty_polls++;
        }
    }
}

static void portAudioEventQueueTrackRecovery(ALEventQueue *evtq) {
    PortAudioEventQueueTrack *track = portAudioEventQueueFind(evtq, FALSE);

    if (track != NULL) {
        if (track->heartbeat_recoveries != ~(u32)0) {
            track->heartbeat_recoveries++;
        }
    }
}

/*
 * Out-of-resource reporting for a lost post, in the exact format the SGI
 * engine emitted from alEvtqPostEvent()'s empty-free-list arm:
 *
 *   [EVTQ] post DROPPED qN(ptr) type=T peak=P of C total=D
 *   [EVTQ] post DROPPED (free list empty) q=ptr type=T total=D
 *
 * Both forms died with that file in the clean-room swap, and the swap's
 * replacement ("[AUDIO] event queue post dropped: queue=... type=...") carries
 * neither the queue index, its high-water mark, its capacity, nor the running
 * drop total. That blinded two gates at once: check_attract_demo.py and
 * check_browser_runtime.py both list "[EVTQ] post DROPPED" as a fatal marker
 * (a dropped post means audio was lost), and check_browser_runtime.py's
 * one-entry-SFX-queue POSITIVE control requires the indexed form — it forces a
 * capacity-1 queue via MDKR_TEST_SFX_EVENT_CAPACITY and asserts a q2 drop with
 * capacity 1 comes back, which is what proves the drop detector itself works
 * rather than merely never firing.
 *
 * Not env-gated: a drop is always worth saying out loud, exactly as before.
 * Capped at 8 reports so a saturating queue cannot flood the log; the total
 * keeps counting past the cap.
 */
#define PORT_AUDIO_EVTQ_DROP_REPORT_MAX 8

static void portAudioEventQueueWarnDrop(ALEventQueue *evtq,
                                        const ALEvent *evt) {
    static s32 s_dropsReported;
    const PortAudioEventQueueTrack *track;
    const char *tail;
    int type = (evt != NULL) ? (int)evt->type : -1;

    if (s_dropsReported >= PORT_AUDIO_EVTQ_DROP_REPORT_MAX) {
        return;
    }
    s_dropsReported++;
    tail = (s_dropsReported == PORT_AUDIO_EVTQ_DROP_REPORT_MAX)
               ? " (further drops counted, not printed)"
               : "";

    track = portAudioEventQueueFind(evtq, FALSE);
    if (track != NULL) {
        fprintf(stderr,
                "[EVTQ] post DROPPED q%d(%p) type=%d peak=%u of %u "
                "total=%d%s\n",
                (int)(track - s_event_queue_tracks), (void *)evtq, type,
                track->high_water, track->capacity, alEvtqDropCount, tail);
    } else {
        fprintf(stderr,
                "[EVTQ] post DROPPED (free list empty) q=%p type=%d "
                "total=%d%s\n",
                (void *)evtq, type, alEvtqDropCount, tail);
    }
    fflush(stderr);
}
#else
static void portAudioEventQueueTrackNew(ALEventQueue *evtq, s32 capacity) {
    (void)evtq;
    (void)capacity;
}

static void portAudioEventQueueTrackInsert(ALEventQueue *evtq) { (void)evtq; }

static void portAudioEventQueueTrackRemove(ALEventQueue *evtq) { (void)evtq; }

static void portAudioEventQueueTrackDrop(ALEventQueue *evtq) { (void)evtq; }

static void portAudioEventQueueTrackEmptyPoll(ALEventQueue *evtq) {
    (void)evtq;
}

static void portAudioEventQueueTrackRecovery(ALEventQueue *evtq) { (void)evtq; }

static void portAudioEventQueueWarnDrop(ALEventQueue *evtq,
                                        const ALEvent *evt) {
    (void)evtq;
    (void)evt;
}
#endif

void alLink(ALLink *element, ALLink *after) {
    if (element == NULL || after == NULL) {
        return;
    }

    element->next = after->next;
    element->prev = after;

    if (after->next != NULL) {
        after->next->prev = element;
    }

    after->next = element;
}

void alUnlink(ALLink *element) {
    if (element == NULL) {
        return;
    }

    if (element->next != NULL) {
        element->next->prev = element->prev;
    }

    if (element->prev != NULL) {
        element->prev->next = element->next;
    }
}

void alEvtqNew(ALEventQueue *evtq, ALEventListItem *items, s32 item_count) {
    s32 i;

    if (evtq == NULL) {
        return;
    }

    evtq->eventCount = 0;
    evtq->allocList.next = NULL;
    evtq->allocList.prev = NULL;
    evtq->freeList.next = NULL;
    evtq->freeList.prev = NULL;

    portAudioEventQueueTrackNew(evtq, item_count);

    if (items == NULL || item_count <= 0) {
        return;
    }

    for (i = 0; i < item_count; i++) {
        alLink((ALLink *)&items[i], &evtq->freeList);
    }
}

ALMicroTime alEvtqNextEvent(ALEventQueue *evtq, ALEvent *evt) {
    ALEventListItem *item;
    ALMicroTime delta = 0;
    OSIntMask mask;

    if (evt == NULL) {
        return 0;
    }

    if (evtq == NULL) {
        memset(evt, 0, sizeof(*evt));
        evt->type = PORT_AUDIO_EVTQ_EMPTY_EVENT;
        return 0;
    }

    mask = osSetIntMask(OS_IM_NONE);
    item = (ALEventListItem *)evtq->allocList.next;

    if (item != NULL) {
        alUnlink((ALLink *)item);
        memcpy(evt, &item->evt, sizeof(*evt));
        alLink((ALLink *)item, &evtq->freeList);
        portAudioEventQueueTrackRemove(evtq);
        delta = item->delta;
    } else {
        memset(evt, 0, sizeof(*evt));
        evt->type = PORT_AUDIO_EVTQ_EMPTY_EVENT;
        portAudioEventQueueTrackEmptyPoll(evtq);
    }

    osSetIntMask(mask);
    return delta;
}

void alEvtqPostEvent(ALEventQueue *evtq, ALEvent *evt, ALMicroTime delta) {
    ALEventListItem *item;
    ALLink *node;
    OSIntMask mask;
    s32 post_at_end;

    if (evtq == NULL || evt == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);

    item = (ALEventListItem *)evtq->freeList.next;
    if (item == NULL) {
        portAudioEventQueueTrackDrop(evtq);
        osSetIntMask(mask);
        portAudioEventQueueWarnDrop(evtq, evt);
        return;
    }

    alUnlink((ALLink *)item);
    memcpy(&item->evt, evt, sizeof(item->evt));
    post_at_end = (delta == AL_EVTQ_END);

    for (node = &evtq->allocList; node != NULL; node = node->next) {
        ALEventListItem *next_item;

        if (node->next == NULL) {
            item->delta = post_at_end ? 0 : delta;
            alLink((ALLink *)item, node);
            break;
        }

        next_item = (ALEventListItem *)node->next;
        if (!post_at_end && delta < next_item->delta) {
            item->delta = delta;
            next_item->delta -= delta;
            alLink((ALLink *)item, node);
            break;
        }

        if (!post_at_end) {
            delta -= next_item->delta;
        }
    }

    portAudioEventQueueTrackInsert(evtq);
    osSetIntMask(mask);
}

void alEvtqFlush(ALEventQueue *evtq) {
    ALLink *node;
    OSIntMask mask;

    if (evtq == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    node = evtq->allocList.next;

    while (node != NULL) {
        ALLink *next = node->next;
        alUnlink(node);
        alLink(node, &evtq->freeList);
        portAudioEventQueueTrackRemove(evtq);
        node = next;
    }

    osSetIntMask(mask);
}

void alEvtqFlushType(ALEventQueue *evtq, s16 type) {
    ALLink *node;
    OSIntMask mask;

    if (evtq == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    node = evtq->allocList.next;

    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;
        ALEventListItem *next_item = (ALEventListItem *)next;

        if (item->evt.type == type) {
            if (next_item != NULL) {
                next_item->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &evtq->freeList);
            portAudioEventQueueTrackRemove(evtq);
        }

        node = next;
    }

    osSetIntMask(mask);
}

ALMicroTime portAudioEventQueueNextOrHeartbeat(ALEventQueue *evtq,
                                               ALEvent *next_event,
                                               s16 heartbeat_type,
                                               ALMicroTime frame_time) {
    ALMicroTime delta = alEvtqNextEvent(evtq, next_event);

    if (next_event != NULL && next_event->type == PORT_AUDIO_EVTQ_EMPTY_EVENT) {
        next_event->type = heartbeat_type;
        portAudioEventQueueTrackRecovery(evtq);
        return frame_time > 0 ? frame_time : AL_USEC_PER_FRAME;
    }

    return delta;
}

void portAudioEventQueueNoteRemoved(ALEventQueue *evtq) {
    OSIntMask mask;

    if (evtq == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    portAudioEventQueueTrackRemove(evtq);
    osSetIntMask(mask);
}

void portAudioEventQueueNoteInserted(ALEventQueue *evtq) {
    OSIntMask mask;

    if (evtq == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    portAudioEventQueueTrackInsert(evtq);
    osSetIntMask(mask);
}

void portAudioEventQueueGetStats(PortAudioEventQueueStats *stats) {
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

#ifdef NATIVE_PORT
    {
        OSIntMask mask = osSetIntMask(OS_IM_NONE);
        s32 i;

        for (i = 0; i < PORT_AUDIO_EVTQ_TRACKED_MAX; i++) {
            const PortAudioEventQueueTrack *track = &s_event_queue_tracks[i];

            if (track->queue == NULL) {
                continue;
            }

            stats->tracked_queues++;
            stats->capacity += track->capacity;
            stats->occupancy += track->occupancy;
            stats->high_water += track->high_water;
            stats->dropped_posts += track->dropped_posts;
            stats->empty_polls += track->empty_polls;
            stats->heartbeat_recoveries += track->heartbeat_recoveries;
        }

        osSetIntMask(mask);
    }
#endif
}
