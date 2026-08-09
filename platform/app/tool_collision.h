// tool_collision.h — the collision viewer (F6).
//
// WHAT IT SHOWS AND WHERE THE DATA COMES FROM. The candidate list the physics
// query itself built: game/src/hasm/collision.c's generate_collision_candidates()
// publishes gCollisionCandidates / gCollisionSurfaces / gNumCollisionCandidates,
// and resolve_collisions() answers out of exactly those entries. This window
// reads that array and nothing else.
//
// It deliberately does NOT re-traverse the level's collision segments to draw
// its own picture of the world. A viewer that re-derives geometry can agree
// with itself while disagreeing with the game, which is the failure mode that
// makes viewers worse than useless — and it is the specific reason DKR's
// fall-through bugs are hard: you cannot tell a geometry bug from a physics bug
// while looking at a second opinion.
//
// WHY THERE ARE NO TRIANGLE OUTLINES. DKR's collision representation is not a
// mesh. A candidate is a tagged handle to either a LevelModelSegment or a
// CollisionFacetPlanes, and a facet carries plane-equation INDICES
// (basePlaneIndex plus three edge bisectors), not vertices — resolve_collisions
// never dereferences a vertex array. Drawing world-space outlines would mean
// reconstructing them from the render mesh, i.e. exactly the second traversal
// this window refuses to make. So the overlay draws the list the query returned.
//
// THE NUMBER TO WATCH. Candidates against the cap. Boss levels 41 and 54 peak
// at 416 of 500, tracked in docs/open-items/collision.md; a run that reaches
// the cap silently DROPS occluders, and the drop is what a fall-through report
// looks like from the outside. Surfacing the headroom live turns a watch metric
// into something a contributor notices before it bites.
#ifndef MDKR64_TOOL_COLLISION_H
#define MDKR64_TOOL_COLLISION_H

// MdkrDevToolDraw-compatible; registered into MDKR_TOOL_COLLISION.
void ToolCollision_draw(bool *open);

#endif  // MDKR64_TOOL_COLLISION_H
