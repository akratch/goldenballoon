/*
 * Native event-queue hardening and diagnostics for the clean-room libaudio
 * compatibility layer.
 *
 * ALEventQueue is an SDK ABI type, so capacity/high-water/drop counters cannot
 * be appended to it. The implementation keeps those diagnostics in a small
 * sidecar keyed by queue address. ALEventQueue.eventCount remains at the
 * stock implementation's zero value; changing that otherwise-dormant ABI field
 * would create an unnecessary behavioral difference for external callers.
 */
#ifndef MGB64_AUDIO_EVENT_QUEUE_H
#define MGB64_AUDIO_EVENT_QUEUE_H

#include <PR/libaudio.h>
#include <ultra64.h>

#define PORT_AUDIO_EVTQ_EMPTY_EVENT ((s16) - 1)

typedef struct PortAudioEventQueueStats_s
{
    u32 tracked_queues;
    u32 capacity;
    u32 occupancy;
    u32 high_water;
    u32 dropped_posts;
    u32 empty_polls;
    u32 heartbeat_recoveries;
} PortAudioEventQueueStats;

/*
 * Fetch the next event. If the queue is empty, schedule the supplied heartbeat
 * directly in next_event and return a positive delay. Keeping the heartbeat in
 * the player's normal next-event slot avoids depending on a free queue node and
 * guarantees the audio driver clock advances even for a zero-capacity queue.
 */
ALMicroTime portAudioEventQueueNextOrHeartbeat(
    ALEventQueue *evtq,
    ALEvent      *next_event,
    s16           heartbeat_type,
    ALMicroTime   frame_time);

/* Account for the few libaudio paths that intentionally splice queue nodes
 * directly instead of going through alEvtqPostEvent/alEvtqNextEvent. */
void portAudioEventQueueNoteRemoved(ALEventQueue *evtq);
void portAudioEventQueueNoteInserted(ALEventQueue *evtq);

/* Aggregate native diagnostics across every queue registered by alEvtqNew. */
void portAudioEventQueueGetStats(PortAudioEventQueueStats *stats);

#endif /* MGB64_AUDIO_EVENT_QUEUE_H */
