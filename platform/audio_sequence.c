/*
 * Clean-room native audio engine: the MIDI sequence readers.
 *
 * Both formats the game ships: the compressed per-track ALCSeq stream and the
 * uncompressed ALSeq stream, each with its event cursor and marker save and
 * restore. Split out of audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

static u8 cseq_track_byte(ALCSeq *seq, u32 track)
{
    u8 value;

    if (seq->curBULen[track] != 0) {
        value = *seq->curBUPtr[track]++;
        seq->curBULen[track]--;
        return value;
    }

    value = *seq->curLoc[track]++;
    if (value == AL_CMIDI_BLOCK_CODE) {
        u8 next = *seq->curLoc[track]++;

        if (next != AL_CMIDI_BLOCK_CODE) {
            u8 backup_lo = *seq->curLoc[track]++;
            u8 length = *seq->curLoc[track]++;
            u32 backup = ((u32)next << 8) | backup_lo;

            seq->curBUPtr[track] = seq->curLoc[track] - (backup + 4);
            seq->curBULen[track] = length;
            value = *seq->curBUPtr[track]++;
            seq->curBULen[track]--;
        }
    }

    return value;
}

static u32 cseq_read_var_len(ALCSeq *seq, u32 track)
{
    u32 value = cseq_track_byte(seq, track);

    if (value & 0x80) {
        u32 next;

        value &= 0x7f;
        do {
            next = cseq_track_byte(seq, track);
            value = (value << 7) + (next & 0x7f);
        } while (next & 0x80);
    }

    return value;
}

static void cseq_track_event(ALCSeq *seq, u32 track, ALEvent *event)
{
    u8 status = cseq_track_byte(seq, track);

    if (status == AL_MIDI_Meta) {
        u8 type = cseq_track_byte(seq, track);

        if (type == AL_MIDI_META_TEMPO) {
            event->type = AL_TEMPO_EVT;
            event->msg.tempo.status = status;
            event->msg.tempo.type = type;
            event->msg.tempo.byte1 = cseq_track_byte(seq, track);
            event->msg.tempo.byte2 = cseq_track_byte(seq, track);
            event->msg.tempo.byte3 = cseq_track_byte(seq, track);
            seq->lastStatus[track] = 0;
        } else if (type == AL_MIDI_META_EOT) {
            u32 mask = 1u << track;

            seq->validTracks ^= mask;
            event->type =
                seq->validTracks != 0 ? AL_TRACK_END : AL_SEQ_END_EVT;
        } else if (type == AL_CMIDI_LOOPSTART_CODE) {
            (void)cseq_track_byte(seq, track);
            (void)cseq_track_byte(seq, track);
            seq->lastStatus[track] = 0;
            event->type = AL_CSP_LOOPSTART;
        } else if (type == AL_CMIDI_LOOPEND_CODE) {
            u8 *cursor = seq->curLoc[track];
            u8 loop_count = *cursor++;
            u8 current_count = *cursor;

            if (current_count == 0) {
                *cursor = loop_count;
                seq->curLoc[track] = cursor + 5;
            } else {
                u32 offset;

                if (current_count != 0xff) {
                    *cursor = current_count - 1;
                }
                cursor++;
                offset = ((u32)cursor[0] << 24) | ((u32)cursor[1] << 16) |
                         ((u32)cursor[2] << 8) | (u32)cursor[3];
                cursor += 4;
                seq->curLoc[track] = cursor - offset;
            }
            seq->lastStatus[track] = 0;
            event->type = AL_CSP_LOOPEND;
        } else {
            event->type = AL_TRACK_END;
        }
    } else {
        event->type = AL_SEQ_MIDI_EVT;
        if (status & 0x80) {
            event->msg.midi.status = status;
            event->msg.midi.byte1 = cseq_track_byte(seq, track);
            seq->lastStatus[track] = status;
        } else {
            event->msg.midi.status = seq->lastStatus[track];
            event->msg.midi.byte1 = status;
        }

        if ((event->msg.midi.status & 0xf0) != AL_MIDI_ProgramChange &&
            (event->msg.midi.status & 0xf0) != AL_MIDI_ChannelPressure) {
            event->msg.midi.byte2 = cseq_track_byte(seq, track);
            if ((event->msg.midi.status & 0xf0) == AL_MIDI_NoteOn) {
                event->msg.midi.duration = cseq_read_var_len(seq, track);
            }
        } else {
            event->msg.midi.byte2 = 0;
        }
    }

}

static int cseq_header_is_native(ALCMidiHdr *header)
{
    return header->division > 0 && header->division < 0x10000;
}

static void cseq_copy_to_marker(ALCSeqMarker *marker, const ALCSeq *seq)
{
    s32 i;

    marker->validTracks = seq->validTracks;
    marker->lastTicks = seq->lastTicks;
    marker->lastDeltaTicks = seq->lastDeltaTicks;

    for (i = 0; i < 16; i++) {
        marker->curLoc[i] = seq->curLoc[i];
        marker->curBUPtr[i] = seq->curBUPtr[i];
        marker->curBULen[i] = seq->curBULen[i];
        marker->lastStatus[i] = seq->lastStatus[i];
        marker->evtDeltaTicks[i] = seq->evtDeltaTicks[i];
    }
}

void alCSeqNew(ALCSeq *seq, u8 *ptr)
{
    ALCMidiHdr *header = (ALCMidiHdr *)ptr;
    int native_header;
    u32 division;
    s32 i;

    if (seq == NULL || ptr == NULL) {
        return;
    }

    native_header = cseq_header_is_native(header);
    seq->base = header;
    seq->validTracks = 0;
    seq->lastDeltaTicks = 0;
    seq->lastTicks = 0;
    seq->deltaFlag = 1;

    division = native_header ? header->division : bank_ctl_u32(ptr + 64);
    header->division = division;

    for (i = 0; i < 16; i++) {
        u32 offset = native_header ? header->trackOffset[i]
                                   : bank_ctl_u32(ptr + (u32)i * 4);

        header->trackOffset[i] = offset;
        seq->lastStatus[i] = 0;
        seq->curBUPtr[i] = NULL;
        seq->curBULen[i] = 0;
        if (offset != 0) {
            seq->validTracks |= 1u << i;
            seq->curLoc[i] = ptr + offset;
            seq->evtDeltaTicks[i] = cseq_read_var_len(seq, (u32)i);
        } else {
            seq->curLoc[i] = NULL;
            seq->evtDeltaTicks[i] = 0;
        }
    }

    seq->qnpt = division != 0 ? 1.0f / (f32)division : 0.0f;
}

void alCSeqNextEvent(ALCSeq *seq, ALEvent *event)
{
    u32 first_time = 0xffffffffu;
    u32 first_track = 0;
    u32 last_ticks;
    s32 i;

    if (seq == NULL || event == NULL) {
        return;
    }

    if (seq->validTracks == 0) {
        event->type = AL_SEQ_END_EVT;
        event->msg.end.ticks = 0;
        return;
    }

    last_ticks = seq->lastDeltaTicks;
    for (i = 0; i < 16; i++) {
        if ((seq->validTracks >> i) & 1u) {
            if (seq->deltaFlag) {
                seq->evtDeltaTicks[i] -= last_ticks;
            }
            if (seq->evtDeltaTicks[i] < first_time) {
                first_time = seq->evtDeltaTicks[i];
                first_track = (u32)i;
            }
        }
    }

    cseq_track_event(seq, first_track, event);

    event->msg.midi.ticks = first_time;
    seq->lastTicks += first_time;
    seq->lastDeltaTicks = first_time;
    if (event->type != AL_TRACK_END) {
        seq->evtDeltaTicks[first_track] +=
            cseq_read_var_len(seq, first_track);
    }
    seq->deltaFlag = 1;
}

char __alCSeqNextDelta(ALCSeq *seq, s32 *delta_ticks)
{
    u32 first_time = 0xffffffffu;
    u32 last_ticks;
    s32 i;

    if (seq == NULL || delta_ticks == NULL || seq->validTracks == 0) {
        return FALSE;
    }

    last_ticks = seq->lastDeltaTicks;
    for (i = 0; i < 16; i++) {
        if ((seq->validTracks >> i) & 1u) {
            if (seq->deltaFlag) {
                seq->evtDeltaTicks[i] -= last_ticks;
            }
            if (seq->evtDeltaTicks[i] < first_time) {
                first_time = seq->evtDeltaTicks[i];
            }
        }
    }

    seq->deltaFlag = 0;
    *delta_ticks = (s32)first_time;
    return TRUE;
}

f32 alCSeqTicksToSec(ALCSeq *seq, s32 ticks, u32 tempo)
{
    if (seq == NULL || seq->base == NULL || seq->base->division == 0) {
        return 0.0f;
    }

    return ((f32)ticks * (f32)tempo) /
           ((f32)seq->base->division * 1000000.0f);
}

u32 alCSeqSecToTicks(ALCSeq *seq, f32 sec, u32 tempo)
{
    if (seq == NULL || seq->base == NULL || tempo == 0) {
        return 0;
    }

    return (u32)(((sec * 1000000.0f) * (f32)seq->base->division) /
                 (f32)tempo);
}

s32 alCSeqGetTicks(ALCSeq *seq)
{
    return seq != NULL ? (s32)seq->lastTicks : 0;
}

void alCSeqNewMarker(ALCSeq *seq, ALCSeqMarker *marker, u32 ticks)
{
    ALCSeq temp_seq;
    ALEvent event;

    if (seq == NULL || marker == NULL || seq->base == NULL) {
        return;
    }

    alCSeqNew(&temp_seq, (u8 *)seq->base);
    do {
        cseq_copy_to_marker(marker, &temp_seq);
        alCSeqNextEvent(&temp_seq, &event);
        if (event.type == AL_SEQ_END_EVT) {
            break;
        }
    } while (temp_seq.lastTicks < ticks);
}

void alCSeqSetLoc(ALCSeq *seq, ALCSeqMarker *marker)
{
    s32 i;

    if (seq == NULL || marker == NULL) {
        return;
    }

    seq->validTracks = marker->validTracks;
    seq->lastTicks = marker->lastTicks;
    seq->lastDeltaTicks = marker->lastDeltaTicks;

    for (i = 0; i < 16; i++) {
        seq->curLoc[i] = marker->curLoc[i];
        seq->curBUPtr[i] = marker->curBUPtr[i];
        seq->curBULen[i] = marker->curBULen[i];
        seq->lastStatus[i] = marker->lastStatus[i];
        seq->evtDeltaTicks[i] = marker->evtDeltaTicks[i];
    }
}

void alCSeqGetLoc(ALCSeq *seq, ALCSeqMarker *marker)
{
    if (seq == NULL || marker == NULL) {
        return;
    }

    cseq_copy_to_marker(marker, seq);
}

#define NATIVE_MIDI_HEADER_MAGIC 0x4d546864u
#define NATIVE_MIDI_TRACK_MAGIC 0x4d54726bu

static u8 seq_read_u8(ALSeq *seq)
{
    return *seq->curPtr++;
}

static u16 seq_read_u16(ALSeq *seq)
{
    u16 value = (u16)seq_read_u8(seq) << 8;

    value |= seq_read_u8(seq);
    return value;
}

static u32 seq_read_u32(ALSeq *seq)
{
    u32 value = (u32)seq_read_u8(seq) << 24;

    value |= (u32)seq_read_u8(seq) << 16;
    value |= (u32)seq_read_u8(seq) << 8;
    value |= seq_read_u8(seq);
    return value;
}

static s32 seq_read_var_len(ALSeq *seq)
{
    s32 value = seq_read_u8(seq);

    if (value & 0x80) {
        s32 next;

        value &= 0x7f;
        do {
            next = seq_read_u8(seq);
            value = (value << 7) + (next & 0x7f);
        } while (next & 0x80);
    }

    return value;
}

static void seq_skip_bytes(ALSeq *seq, s32 byte_count)
{
    if (byte_count <= 0) {
        return;
    }

    seq->curPtr += byte_count;
}

void alSeqNew(ALSeq *seq, u8 *ptr, s32 len)
{
    u16 division;

    if (seq == NULL || ptr == NULL) {
        return;
    }

    seq->base = ptr;
    seq->len = len;
    seq->lastStatus = 0;
    seq->lastTicks = 0;
    seq->curPtr = ptr;
    seq->trackStart = ptr;
    seq->division = 0;
    seq->qnpt = 0.0f;

    if (len < 22 || seq_read_u32(seq) != NATIVE_MIDI_HEADER_MAGIC) {
        return;
    }

    (void)seq_read_u32(seq);
    if (seq_read_u16(seq) != 0 || seq_read_u16(seq) != 1) {
        return;
    }

    division = seq_read_u16(seq);
    if (division & 0x8000) {
        return;
    }

    seq->division = (s16)division;
    seq->qnpt = division != 0 ? 1.0f / (f32)division : 0.0f;

    if (seq_read_u32(seq) != NATIVE_MIDI_TRACK_MAGIC) {
        return;
    }

    (void)seq_read_u32(seq);
    seq->trackStart = seq->curPtr;
}

void alSeqNextEvent(ALSeq *seq, ALEvent *event)
{
    u8 status;
    s32 delta_ticks;

    if (seq == NULL || event == NULL) {
        return;
    }

    if (seq->curPtr >= seq->base + seq->len) {
        event->type = AL_SEQ_END_EVT;
        event->msg.end.ticks = 0;
        return;
    }

    delta_ticks = seq_read_var_len(seq);
    seq->lastTicks += delta_ticks;
    status = seq_read_u8(seq);

    if (status == 0xf0 || status == 0xf7) {
        s32 payload_len = seq_read_var_len(seq);

        seq_skip_bytes(seq, payload_len);
        alSeqNextEvent(seq, event);
        return;
    }

    if (status == AL_MIDI_Meta) {
        u8 type = seq_read_u8(seq);

        if (type == AL_MIDI_META_TEMPO) {
            event->type = AL_TEMPO_EVT;
            event->msg.tempo.ticks = delta_ticks;
            event->msg.tempo.status = status;
            event->msg.tempo.type = type;
            event->msg.tempo.len = seq_read_u8(seq);
            event->msg.tempo.byte1 = seq_read_u8(seq);
            event->msg.tempo.byte2 = seq_read_u8(seq);
            event->msg.tempo.byte3 = seq_read_u8(seq);
        } else if (type == AL_MIDI_META_EOT) {
            event->type = AL_SEQ_END_EVT;
            event->msg.end.ticks = delta_ticks;
            event->msg.end.status = status;
            event->msg.end.type = type;
            event->msg.end.len = seq_read_u8(seq);
        } else {
            s32 payload_len = seq_read_var_len(seq);

            seq_skip_bytes(seq, payload_len);
            alSeqNextEvent(seq, event);
            return;
        }

        seq->lastStatus = 0;
        return;
    }

    event->type = AL_SEQ_MIDI_EVT;
    event->msg.midi.ticks = delta_ticks;
    if (status & 0x80) {
        event->msg.midi.status = status;
        event->msg.midi.byte1 = seq_read_u8(seq);
        seq->lastStatus = status;
    } else {
        event->msg.midi.status = seq->lastStatus;
        event->msg.midi.byte1 = status;
    }

    if ((event->msg.midi.status & 0xf0) != AL_MIDI_ProgramChange &&
        (event->msg.midi.status & 0xf0) != AL_MIDI_ChannelPressure) {
        event->msg.midi.byte2 = seq_read_u8(seq);
    } else {
        event->msg.midi.byte2 = 0;
    }
}

char __alSeqNextDelta(ALSeq *seq, s32 *delta_ticks)
{
    u8 *saved;

    if (seq == NULL || delta_ticks == NULL || seq->curPtr == NULL ||
        seq->base == NULL || seq->curPtr >= seq->base + seq->len) {
        return FALSE;
    }

    saved = seq->curPtr;
    *delta_ticks = seq_read_var_len(seq);
    seq->curPtr = saved;
    return TRUE;
}

f32 alSeqTicksToSec(ALSeq *seq, s32 ticks, u32 tempo)
{
    if (seq == NULL || seq->division == 0) {
        return 0.0f;
    }

    return ((f32)ticks * (f32)tempo) /
           ((f32)seq->division * 1000000.0f);
}

u32 alSeqSecToTicks(ALSeq *seq, f32 sec, u32 tempo)
{
    if (seq == NULL || tempo == 0) {
        return 0;
    }

    return (u32)(((sec * 1000000.0f) * (f32)seq->division) /
                 (f32)tempo);
}

void alSeqNewMarker(ALSeq *seq, ALSeqMarker *marker, u32 ticks)
{
    ALEvent event;
    u8 *saved_ptr;
    u8 *last_ptr;
    s32 saved_ticks;
    s32 last_ticks;
    s16 saved_status;
    s16 last_status;

    if (seq == NULL || marker == NULL) {
        return;
    }

    if (ticks == 0) {
        marker->curPtr = seq->trackStart;
        marker->lastStatus = 0;
        marker->lastTicks = 0;
        marker->curTicks = 0;
        return;
    }

    saved_ptr = seq->curPtr;
    saved_status = seq->lastStatus;
    saved_ticks = seq->lastTicks;

    seq->curPtr = seq->trackStart;
    seq->lastStatus = 0;
    seq->lastTicks = 0;

    do {
        last_ptr = seq->curPtr;
        last_status = seq->lastStatus;
        last_ticks = seq->lastTicks;

        alSeqNextEvent(seq, &event);
        if (event.type == AL_SEQ_END_EVT) {
            last_ptr = seq->curPtr;
            last_status = seq->lastStatus;
            last_ticks = seq->lastTicks;
            break;
        }
    } while ((u32)seq->lastTicks < ticks);

    marker->curPtr = last_ptr;
    marker->lastStatus = last_status;
    marker->lastTicks = last_ticks;
    marker->curTicks = seq->lastTicks;

    seq->curPtr = saved_ptr;
    seq->lastStatus = saved_status;
    seq->lastTicks = saved_ticks;
}

s32 alSeqGetTicks(ALSeq *seq)
{
    return seq != NULL ? seq->lastTicks : 0;
}

void alSeqSetLoc(ALSeq *seq, ALSeqMarker *marker)
{
    if (seq == NULL || marker == NULL) {
        return;
    }

    seq->curPtr = marker->curPtr;
    seq->lastStatus = marker->lastStatus;
    seq->lastTicks = marker->lastTicks;
}

void alSeqGetLoc(ALSeq *seq, ALSeqMarker *marker)
{
    if (seq == NULL || marker == NULL) {
        return;
    }

    marker->curPtr = seq->curPtr;
    marker->lastStatus = seq->lastStatus;
    marker->lastTicks = seq->lastTicks;
}
