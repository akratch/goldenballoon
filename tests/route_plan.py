#!/usr/bin/env python3
"""Waypoint following that fails loudly instead of oscillating — S9 M2.

Why this exists
---------------
The lobby boss-rematch door is not driven by any gate, and the reason is
recorded in `tests/fixtures/README.md` § "Residual manual acceptance": measured
in the Dino Domain lobby with all door bits open, the kart stalls at
(-1295, 685), 1,240 units short of the boss exit at (-777, 1812), and the drive
hook's reverse-and-retry **oscillates there indefinitely**.

Read that again, because it decides what this module is. The problem is not
that the exit is unreachable — it is reachable from the spawn the player returns
to after a race. The problem is that the drive hook has no route memory and no
give-up condition, so a blocked heading is re-attempted forever and the run
neither arrives nor reports. A route that hangs is worse than a route that
fails: a failure names a waypoint and a distance, and a hang names nothing.

So the loud failures below are the deliverable, not the driving. Three of them:

  budget       Each waypoint carries a frame budget. Exceeding it raises with
               the waypoint index, its target, the closest approach actually
               achieved, and the distance still remaining. "Did not arrive" is
               useless; "got within 1,240 units of (-777, 1812) and stopped
               closing" is a lead.

  oscillation  If the position keeps revisiting one small neighbourhood, the
               route is stuck in exactly the reverse-and-retry loop that
               motivated this module. Detected and raised on before the budget
               runs out, because the budget would eventually catch it but would
               waste the whole budget doing so and would report the wrong
               reason.

  regression   If closest approach stops improving for a sustained stretch, the
               route is being held off rather than making slow progress. This is
               distinct from oscillation: a kart pinned against a wall by a
               constant input does not revisit a neighbourhood, it simply stops
               moving.

None of the three is a heuristic about DKR. They are all statements about the
position trace, so this module needs no game, and its own self-test below drives
it from a synthetic position source with no ROM, no build and no window.

Usage
-----
    from route_plan import Waypoint, RouteFollower, RouteFailure

    route = RouteFollower([
        Waypoint(x=-1295.0, z=685.0,  radius=60.0, budget_frames=600),
        Waypoint(x=-777.0,  z=1812.0, radius=80.0, budget_frames=1200),
    ])
    for frame, (x, z) in enumerate(positions):
        if route.observe(frame, x, z):     # True once every waypoint is reached
            break
    route.require_complete()               # raises RouteFailure with the detail

`observe()` never blocks and never drives; the caller owns the input. This
module only watches, and decides when watching has stopped being useful.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field


class RouteFailure(AssertionError):
    """A route that did not arrive, with enough detail to act on."""


@dataclass(frozen=True)
class Waypoint:
    """One target on the ground plane.

    `radius` is arrival distance in world units. `budget_frames` is how long the
    follower will watch for this waypoint before declaring failure; it is per
    waypoint rather than per route so a long straight and a tight corner do not
    have to share one number.
    """

    x: float
    z: float
    radius: float
    budget_frames: int
    name: str = ""

    def distance_from(self, x: float, z: float) -> float:
        return math.hypot(x - self.x, z - self.z)

    def label(self) -> str:
        return self.name or f"({self.x:.0f}, {self.z:.0f})"


@dataclass
class _Progress:
    entered_frame: int
    closest: float = math.inf
    closest_frame: int = -1
    # How many times the kart has ENTERED each coarse grid cell while chasing
    # this waypoint -- transitions in, not frames spent.
    #
    # Counting frames instead was the first implementation and it was wrong: a
    # genuine slow approach crossing one 48-unit cell at 1 unit/frame racks up
    # 48 samples in that cell and reads as stuck. Oscillation is *leaving* a
    # cell and *coming back*, which is exactly what reverse-and-retry does and
    # what a steady traversal never does. The self-test's straight-approach case
    # is what caught this.
    cell_entries: dict[tuple[int, int], int] = field(default_factory=dict)
    last_cell: tuple[int, int] | None = None


class RouteFollower:
    """Watches a position trace against an ordered waypoint list.

    Deliberately does not drive. The existing harnesses own input; wiring this
    to also press buttons would couple the give-up logic to one input mechanism
    and make it untestable without the game.
    """

    #: Side of the oscillation-detection grid cell, in world units. A kart
    #: genuinely travelling leaves a cell within a few frames; one wedged
    #: against geometry re-enters the same cell repeatedly.
    CELL_SIZE = 48.0

    #: Re-entering one cell this many times while chasing a single waypoint is
    #: treated as stuck. Chosen well above the handful of visits an ordinary
    #: cornering line produces, so a slow but real approach is not cut off.
    CELL_REVISIT_LIMIT = 24

    #: Frames without improving closest approach before the route is declared
    #: held off. Distinct from oscillation: a kart pinned by constant input does
    #: not revisit cells, it stops moving at all.
    NO_PROGRESS_FRAMES = 300

    def __init__(self, waypoints: list[Waypoint]) -> None:
        if not waypoints:
            raise ValueError("a route needs at least one waypoint")
        self.waypoints = list(waypoints)
        self.index = 0
        self.trace_len = 0
        self._progress = _Progress(entered_frame=0)
        self.reached: list[tuple[int, Waypoint]] = []

    # -- observation ------------------------------------------------------

    def observe(self, frame: int, x: float, z: float) -> bool:
        """Feed one position sample. Returns True when the route is complete.

        Raises RouteFailure the moment the current waypoint is judged
        unreachable, rather than at the end of the run, so the failing frame is
        the one reported.
        """
        if self.complete:
            return True
        self.trace_len += 1
        wp = self.waypoints[self.index]
        distance = wp.distance_from(x, z)

        if distance <= wp.radius:
            self.reached.append((frame, wp))
            self.index += 1
            if self.complete:
                return True
            self._progress = _Progress(entered_frame=frame)
            return False

        prog = self._progress
        if distance < prog.closest:
            prog.closest = distance
            prog.closest_frame = frame

        cell = (int(x // self.CELL_SIZE), int(z // self.CELL_SIZE))
        if cell != prog.last_cell:
            prog.last_cell = cell
            entries = prog.cell_entries.get(cell, 0) + 1
            prog.cell_entries[cell] = entries
            if entries >= self.CELL_REVISIT_LIMIT:
                raise RouteFailure(
                    self._detail(
                        wp, frame, distance,
                        f"oscillating: entered the {self.CELL_SIZE:.0f}-unit "
                        f"cell at {cell} {entries} separate times without "
                        f"arriving. This is the reverse-and-retry loop, not a "
                        f"slow approach -- a route that is merely slow crosses "
                        f"a cell once."))

        elapsed = frame - prog.entered_frame
        stalled = frame - prog.closest_frame
        if prog.closest_frame >= 0 and stalled >= self.NO_PROGRESS_FRAMES:
            raise RouteFailure(
                self._detail(
                    wp, frame, distance,
                    f"held off: closest approach has not improved for "
                    f"{stalled} frames."))

        if elapsed >= wp.budget_frames:
            raise RouteFailure(
                self._detail(
                    wp, frame, distance,
                    f"budget exhausted after {elapsed} frames "
                    f"(limit {wp.budget_frames})."))
        return False

    # -- results ----------------------------------------------------------

    @property
    def complete(self) -> bool:
        return self.index >= len(self.waypoints)

    def require_complete(self) -> None:
        """Raise unless every waypoint was reached.

        Call this after the trace ends. A trace that simply ran out is a
        distinct failure from one the follower rejected mid-flight, and says so.
        """
        if self.complete:
            return
        wp = self.waypoints[self.index]
        prog = self._progress
        closest = "never measured" if prog.closest is math.inf \
            else f"{prog.closest:.0f} units"
        raise RouteFailure(
            f"route ended with waypoint {self.index} {wp.label()} unreached "
            f"after {self.trace_len} samples; closest approach {closest}. "
            f"The trace ran out rather than the follower giving up, so the "
            f"frame budget was never the constraint -- the caller stopped "
            f"driving first.")

    def summary(self) -> str:
        rows = [f"  waypoint {i} {wp.label()} reached at frame {f}"
                for i, (f, wp) in enumerate(self.reached)]
        head = (f"route: {len(self.reached)}/{len(self.waypoints)} waypoints "
                f"over {self.trace_len} samples")
        return "\n".join([head, *rows])

    # -- internals --------------------------------------------------------

    def _detail(self, wp: Waypoint, frame: int, distance: float,
                why: str) -> str:
        prog = self._progress
        closest = "never measured" if prog.closest is math.inf \
            else f"{prog.closest:.0f}"
        return (
            f"waypoint {self.index} {wp.label()} not reached at frame {frame}: "
            f"{why}\n"
            f"  target        ({wp.x:.0f}, {wp.z:.0f}) radius {wp.radius:.0f}\n"
            f"  distance now  {distance:.0f} units\n"
            f"  closest ever  {closest} units at frame {prog.closest_frame}\n"
            f"  reached so far: {len(self.reached)}/{len(self.waypoints)}")


# -- self-test ------------------------------------------------------------
# Runs with no ROM, no build and no window: the position source is synthetic, so
# every give-up rule is exercised directly rather than inferred from a race.

def _expect(cond: bool, what: str, failures: list[str]) -> None:
    if cond:
        print(f"ok   {what}")
    else:
        print(f"FAIL {what}")
        failures.append(what)


def _self_test() -> int:
    failures: list[str] = []

    # 1. A straight approach arrives.
    route = RouteFollower([Waypoint(100.0, 0.0, radius=10.0, budget_frames=200)])
    done = False
    for frame in range(200):
        done = route.observe(frame, float(frame), 0.0)
        if done:
            break
    _expect(done and route.complete, "a straight approach reaches its waypoint",
            failures)

    # 2. Waypoints are consumed in order, and a later one does not count early.
    route = RouteFollower([
        Waypoint(50.0, 0.0, radius=5.0, budget_frames=500),
        Waypoint(0.0, 0.0, radius=5.0, budget_frames=500),
    ])
    # Start ON the second waypoint. It must not be credited before the first.
    route.observe(0, 0.0, 0.0)
    _expect(route.index == 0,
            "standing on a later waypoint does not skip an earlier one",
            failures)

    # 3. Oscillation is caught, and caught BEFORE the budget would have.
    #    Modelled as the recorded lobby symptom: reverse-and-retry, i.e. driving
    #    back and forth ACROSS a cell boundary forever. Jittering inside one
    #    cell is a different thing and is caught by the held-off rule instead.
    route = RouteFollower([Waypoint(1812.0, 0.0, radius=50.0,
                                    budget_frames=100000)])
    caught = ""
    try:
        for frame in range(100000):
            route.observe(frame, 600.0 if frame % 2 else 700.0, 0.0)
    except RouteFailure as exc:
        caught = str(exc)
    _expect("oscillating" in caught,
            "an indefinite reverse-and-retry loop is reported as oscillation",
            failures)
    _expect("closest ever" in caught and "1812" in caught,
            "the oscillation report names the target and the closest approach",
            failures)

    # 4. Being held off is distinguished from oscillating: the kart moves
    #    steadily along a wall, never revisiting a cell, never getting closer.
    route = RouteFollower([Waypoint(0.0, 0.0, radius=10.0,
                                    budget_frames=100000)])
    caught = ""
    try:
        for frame in range(100000):
            route.observe(frame, 500.0, float(frame) * 10.0)
    except RouteFailure as exc:
        caught = str(exc)
    _expect("held off" in caught,
            "steady motion that never closes is reported as held off, "
            "not as oscillation", failures)

    # 5. The budget bites when neither other rule does: a slow but genuine
    #    approach that simply runs out of frames.
    route = RouteFollower([Waypoint(10000.0, 0.0, radius=10.0,
                                    budget_frames=50)])
    caught = ""
    try:
        for frame in range(1000):
            route.observe(frame, float(frame) * 60.0, 0.0)
    except RouteFailure as exc:
        caught = str(exc)
    _expect("budget exhausted" in caught,
            "a genuine approach that runs out of frames reports the budget",
            failures)

    # 6. A trace that simply ends is a DIFFERENT failure from one the follower
    #    rejected -- the caller stopped driving, the budget was never reached.
    route = RouteFollower([Waypoint(10000.0, 0.0, radius=10.0,
                                    budget_frames=100000)])
    for frame in range(10):
        route.observe(frame, float(frame), 0.0)
    caught = ""
    try:
        route.require_complete()
    except RouteFailure as exc:
        caught = str(exc)
    _expect("ran out rather than the follower giving up" in caught,
            "a truncated trace is reported as the caller stopping, not a budget",
            failures)

    # 7. An empty waypoint list is a programming error, not a passing route.
    try:
        RouteFollower([])
        _expect(False, "an empty route is rejected", failures)
    except ValueError:
        _expect(True, "an empty route is rejected", failures)

    print()
    if failures:
        print(f"FAILURES: {len(failures)}")
        return 1
    print("all route_plan assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(_self_test())
