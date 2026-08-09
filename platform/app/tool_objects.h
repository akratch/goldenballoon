// tool_objects.h — the live object viewer (F7).
//
// It lists behaviour id, position, flags and particle status for every entry in
// the object list, sourced through platform/sim_hash_view.h — which is
// implemented inside platform/sim_hash.c and walks the SAME objGetObjList()
// array, in the same order, through the same presentation-companion filter as
// the v3 [SIMHASH] walk.
//
// That is the whole design constraint. The alternatives were a second extern of
// gObjPtrList here, or the presentation snapshot's published object array;
// either would let this window and the authoritative hash disagree about what
// is live, and a viewer that can disagree with the game is worse than no
// viewer. Sourcing from the hash's own walk makes the disagreement impossible
// rather than merely unlikely.
#ifndef MDKR64_TOOL_OBJECTS_H
#define MDKR64_TOOL_OBJECTS_H

// MdkrDevToolDraw-compatible; registered into MDKR_TOOL_OBJECTS.
void ToolObjects_draw(bool *open);

#endif  // MDKR64_TOOL_OBJECTS_H
