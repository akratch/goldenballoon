#!/usr/bin/env python3
"""Normal Modern held-shot behaviour under injected faults (Ancient Lake route).

Every arm sets MDKR_TEST_CAMERA_HELD_FAULT. The runtime seam finds its own
held-shot episode and reports the tick it fired on, so no arm pins a route
tick that an unrelated timing or tuning change would move:

  projection     the projection latch fails inside an active held shot: the
                 tick publishes a validated fallback cut and the chain is
                 retired rather than continued across the missing projection
  query          the first exact probe of the held chain's OWN proof is
                 unprovable: the chain rejects that candidate, the tick keeps
                 the degraded-probe mark, and publication stays safe
  false_clear    a corridor false-clear run one tick shorter than the release
                 hold: the held eye must not move (rigid pivot translation,
                 unchanged orientation) and the shot must not start recovering
  false_contact  one contact inside a released held recovery: the MOTION-01
                 census must count the re-engagement and the motion gate must
                 fail -- the positive control that held recovery is censused
                 as clear, not as part of the blocked span

Two more arms lower the held-shot bounds through MDKR_TEST_CAMERA_HELD_BOUNDS,
because the route's natural held shots stay inside the shipped ones:

  dwell_bound       a held shot ends once it has held for the bound
  divergence_bound  a held view may not turn further from the authored view
                    than the bound

and both require that the ordinary boom path took the tick a bound refused,
that a published shot's handover tick was published as a cut, that no held
shot re-seeded before that correction released, that the census
measured every published held tick inside the bound, and that the binary
reported the bound it ran with.

  leash             a 2-degree leash (hard bound lifted): every holding view
                    past it must be turning back at the full held return
                    rate, and the census must count the leashed ticks
  soft_decline      drives the 3P route with MDKR_TEST_CAMERA_SOFT_STATIC, so
                    static blockers qualify for the soft-occluder decline: a
                    held shot may not freeze the view for a blocker the boom
                    would not retract for. Every tick the held path deferred
                    must publish no held shot and must be declined by the
                    ordinary path. (No shipped content reaches this branch.)

Every arm also requires safe publication on every tick, well-formed chain
evidence, and the release-hold invariant on the route's own held episodes:
recovery starts on exactly the release-hold-th consecutive clear tick.
Uses the motion runner's isolated private save/config directories.
"""
import argparse
import re
from pathlib import Path

import check_camera_chain_evidence as chain_evidence
import check_camera_motion_quality as motion

# Mirrors MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS; verified against the
# census's own release_hold window on every run so the two cannot drift.
RELEASE_HOLD_TICKS = 9
# The grazing extension's bound: a served hold may wait this many more clear ticks,
# and only while the corridor still grazes geometry (near=1). It is the
# MOTION-01 chatter window, which the runtime pins with a static assertion.
NEAR_CONTACT_EXTENSION_TICKS = motion.OSCILLATION_TICKS
ARMS = {
    "projection": "projection",
    "query": "query",
    "false_clear": f"false_clear:{RELEASE_HOLD_TICKS - 1}",
    "false_contact": "false_contact",
}
# Bound arms: (dwell seconds, divergence degrees). Each lowers one bound well
# under what the route's held shots reach and lifts the other out of the way.
BOUND_ARMS = {
    "dwell_bound": (0.2, 180.0),
    "divergence_bound": (3600.0, 2.0),
}
# Leash arm: hard bounds lifted, leash at 2 degrees, so every held shot turns.
LEASH_ARM = (3600.0, 180.0, 2.0)
# The held return rate at the gate's Enhanced cadence, in degrees per tick:
# 90 degrees per second over 60 ticks, floored to whole s16 angle units.
LEASH_FULL_STEP = int(16384 / 60) * 360.0 / 65536.0
# The 3P route is where held shots meet blockers the decline would release.
SOFT_ROUTE = "3p-tt-spectate"
SOFT_STATIC_EXTENT = "1"
# Rigid translation is exact up to float rounding at track coordinates.
RIGID_TOLERANCE = 0.01

ARMED = re.compile(r"camera_held_fault armed kind=(\w+) tick=(\d+) slot=(\d+) "
                   r"ticks=(\d+) prior_length=(\d+)$")
PROBE = re.compile(r"camera_held_chain_probe tick=(\d+) slot=(\d+) continued=([01]) "
                   r"prior_length=(\d+) valid=([01]) .* recovering=([01]) "
                   r"remaining=(\S+) returned=([01]) rejection=(\w+) contact=([01]) "
                   r"near=([01]) dwell=(\S+) divergence=(\S+) leash=(\S+)$")
PUBLISHED = re.compile(r"camera_held_shot_published tick=(\d+) slot=(\d+) "
                       r"frame=\d+ active=([01]) dwell=(\S+) divergence=(\S+)$")
VEC = r"\(([^,]+),([^,]+),([^)]+)\)"
TICK = re.compile(r"^camera_\w+(?: \w+(?:=\S+)?)*? tick=(\d+)\b")


def epochs(output):
    """Split a trace where the runtime tick serial restarts (level loads).

    Rows of one tick print together and ticks only grow inside one level, so a
    smaller tick than the last one opens the next epoch. Tick-keyed evidence is
    only meaningful inside its own epoch.
    """
    split, last = [[]], -1
    for row in output.splitlines():
        match = TICK.match(row)
        if match is not None:
            tick = int(match[1])
            if tick < last:
                split.append([])
            last = tick
        split[-1].append(row)
    return ["\n".join(rows) for rows in split]


def _rows(output, prefix):
    return [row for row in output.splitlines() if row.startswith(prefix)]


def armed(output, kind):
    rows = _rows(output, "camera_held_fault armed ")
    if len(rows) != 1:
        raise ValueError(f"expected one armed {kind} fault, saw {len(rows)}")
    match = ARMED.fullmatch(rows[0])
    if match is None or match[1] != kind:
        raise ValueError(f"malformed or wrong-kind fault row: {rows[0]}")
    return int(match[2]), int(match[3]), int(match[4])


def probes(output):
    """{(tick, slot): parsed held-chain row} for every held proof attempt."""
    parsed = {}
    for row in _rows(output, "camera_held_chain_probe "):
        match = PROBE.fullmatch(row)
        if match is None:
            raise ValueError(f"malformed held chain row: {row[:160]}")
        key = (int(match[1]), int(match[2]))
        if key in parsed:
            raise ValueError(f"duplicate held chain row at {key}")
        parsed[key] = dict(continued=int(match[3]), valid=int(match[5]),
                           recovering=int(match[6]), returned=int(match[8]),
                           rejection=match[9], contact=int(match[10]),
                           near=int(match[11]), dwell=float(match[12]),
                           divergence=float(match[13]), leash=float(match[14]))
    return parsed


def published(output, measures=None):
    """{(tick, slot): active}; `measures` collects (dwell, divergence) too."""
    active = {}
    for row in _rows(output, "camera_held_shot_published "):
        match = PUBLISHED.fullmatch(row)
        if match is None:
            raise ValueError(f"malformed held publication: {row}")
        active[(int(match[1]), int(match[2]))] = int(match[3])
        if measures is not None and match[3] == "1":
            measures[(int(match[1]), int(match[2]))] = (float(match[4]), float(match[5]))
    return active


def details(output, prefix, tick, slot):
    rows = [row for row in output.splitlines()
            if row.startswith(f"{prefix} tick={tick} ") and
            f" physical_slot={slot} " in row]
    if len(rows) != 1:
        raise ValueError(f"expected one {prefix} row at tick {tick}, saw {len(rows)}")
    return rows[0]


def vector(row, name):
    match = re.search(rf"\b{name}={VEC}", row)
    if match is None:
        raise ValueError(f"missing {name} in {row[:120]}")
    return tuple(float(value) for value in match.groups())


def common(output):
    summaries = [row for row in output.splitlines()
                 if "camera_obstruction_observe summary" in row]
    if not summaries:
        raise ValueError("no publication evidence")
    for row in summaries:
        if not re.search(r"resolved=\{corrected=\d+ penetrated=0 invalid=0 degraded=0\}", row):
            raise ValueError("unsafe publication summary")
        if "projection_mismatches=0 " not in row:
            raise ValueError("projection mismatch")
    census = [row for row in output.splitlines() if motion.MOTION_SUMMARY in row]
    if not census or motion.parse_summary(census[-1])["release_hold_window"] != RELEASE_HOLD_TICKS:
        raise ValueError("census release-hold window disagrees with this gate")
    chain_evidence.audit(output.splitlines())
    for epoch in epochs(output):
        release_hold_invariant(probes(epoch))
    return summaries


def release_hold_invariant(rows):
    """Held recovery starts exactly where the release hysteresis allows.

    That is the hold-th consecutive clear tick, unless the grazing extension read
    the corridor as still grazing (near=1) on that tick and every one after it,
    in which case it is the first clear tick that is not grazing, and never
    later than hold + extension bound. Returns the number of releases checked.
    """
    starts = 0
    for (tick, slot), row in sorted(rows.items()):
        previous = rows.get((tick - 1, slot))
        if not (row["valid"] and row["recovering"] and row["continued"] and
                previous is not None and previous["valid"] and
                not previous["recovering"]):
            continue
        held = 0
        while True:
            earlier = rows.get((tick - 1 - held, slot))
            if earlier is None or not earlier["valid"] or earlier["contact"] or \
                    earlier["recovering"]:
                break
            held += 1
        run = held + 1  # the releasing tick is the run's last clear tick
        latest = RELEASE_HOLD_TICKS + NEAR_CONTACT_EXTENSION_TICKS
        if not RELEASE_HOLD_TICKS <= run <= latest:
            raise ValueError(f"held recovery at tick {tick} released after a "
                             f"{run}-tick clear run, outside "
                             f"[{RELEASE_HOLD_TICKS}, {latest}]")
        for index in range(RELEASE_HOLD_TICKS, run):
            if not rows[(tick - run + index, slot)]["near"]:
                raise ValueError(f"held recovery at tick {tick} waited past a "
                                 f"served hold that was not grazing")
        if row["near"] and run != latest:
            raise ValueError(f"held recovery at tick {tick} released while grazing "
                             f"before the extension's bound")
        starts += 1
    for (tick, slot), row in rows.items():
        if row["valid"] and row["recovering"] and row["contact"]:
            raise ValueError(f"tick {tick} recovered on a contact")
    return starts


def verify_bound(output, kind):
    """A lowered held-shot bound ends held shots and hands over cleanly."""
    if kind not in BOUND_ARMS:
        raise ValueError("unknown bound arm")
    common(output)
    dwell_bound, divergence_bound = BOUND_ARMS[kind]
    reason = "dwell" if kind == "dwell_bound" else "divergence"
    census = motion.parse_summary(
        [row for row in output.splitlines() if motion.MOTION_SUMMARY in row][-1])
    if abs(census["held_dwell_bound_ms"] - dwell_bound * 1000.0) > 0.05 or \
            abs(census["held_divergence_bound_deg"] - divergence_bound) > 0.0005:
        raise ValueError("the runtime did not run with the lowered bound")
    if census["max_held_dwell_ms"] > (dwell_bound + 1.0 / 60.0) * 1000.0 or \
            census["max_held_divergence_deg"] > divergence_bound + 0.01:
        raise ValueError("the census measured a published held shot past its bound: "
                         f"{census['max_held_dwell_ms']:.1f}ms "
                         f"{census['max_held_divergence_deg']:.2f}deg")
    if census["held_bound_exits"] < 1:
        raise ValueError(f"no held shot reached the lowered {reason} bound")
    exits = 0
    for epoch in epochs(output):
        rows = probes(epoch)
        measures = {}
        active = published(epoch, measures)
        for (tick, slot), (dwell, divergence) in measures.items():
            if dwell > dwell_bound + 1e-4 or divergence > divergence_bound + 1e-3:
                raise ValueError(f"tick {tick} published a held shot past its bound")
        for (tick, slot), row in sorted(rows.items()):
            if row["valid"] or row["rejection"] != reason:
                continue
            exits += 1
            detail = details(epoch, "camera_obstruction_observe detail", tick, slot)
            if row["continued"] and not re.search(r"resolve=\{[^}]* discontinuity=1 ", detail):
                raise ValueError(f"tick {tick}: a bounded held shot's handover was not a cut")
            if not row["continued"]:
                # A refused seed published nothing; later seeds are judged
                # against the bound again, so only the handover is checked.
                if active.get((tick, slot)) is not None:
                    raise ValueError(f"tick {tick} published the held seed its bound refused")
                continue
            if active.get((tick, slot)) is not None:
                raise ValueError(f"tick {tick} published the held shot its bound refused")
            detail = details(epoch, "camera_obstruction_observe detail", tick, slot)
            if not re.search(r"resolve=\{status=[0-3] valid=1 ", detail) or \
                    not re.search(r" clearance=0 ", detail):
                raise ValueError(f"the ordinary path did not safely take tick {tick}")
            # No held shot until the ordinary correction has released.
            later = tick + 1
            while True:
                if active.get((later, slot)) is not None:
                    raise ValueError(f"a held shot re-seeded at {later} before the "
                                     f"correction that replaced it released")
                try:
                    row_later = details(epoch, "camera_obstruction_observe detail", later, slot)
                except ValueError:
                    break
                if re.search(r"resolve=\{status=0 valid=1 corrected=0 ", row_later):
                    break
                later += 1
    if exits == 0:
        raise ValueError(f"no held proof was refused by its {reason} bound")
    return exits


def verify_soft(output):
    """The held path defers to the soft-occluder decline the boom applies."""
    summaries = [row for row in output.splitlines()
                 if "camera_obstruction_observe summary" in row]
    if not summaries:
        raise ValueError("no publication evidence")
    for row in summaries:
        if not re.search(r"resolved=\{corrected=\d+ penetrated=0 invalid=0 degraded=0\}", row):
            raise ValueError("unsafe publication summary")
    chain_evidence.audit(output.splitlines())
    deferred = 0
    for epoch in epochs(output):
        rows = probes(epoch)
        active = published(epoch)
        for row in _rows(epoch, "camera_held_soft_decline "):
            match = re.fullmatch(r"camera_held_soft_decline tick=(\d+) slot=(\d+) "
                                 r"blocker=(\d+) extent=(\S+)", row)
            if match is None:
                raise ValueError(f"malformed soft-decline row: {row}")
            tick, slot = int(match[1]), int(match[2])
            if (tick, slot) in active or (tick, slot) in rows:
                raise ValueError(f"tick {tick} held the view for a declined blocker")
            detail = details(epoch, "camera_obstruction_observe detail", tick, slot)
            if "soft_blocker={seen=1 declined=1 " not in detail:
                raise ValueError(f"tick {tick} deferred but the boom did not decline")
            deferred += 1
    if deferred == 0:
        raise ValueError("the held path never deferred to the soft-occluder decline")
    return deferred


def verify_leash(output):
    """Past the leash a holding view turns back at the full held return rate."""
    common(output)
    _dwell, _divergence, leash_bound = LEASH_ARM
    census = motion.parse_summary(
        [row for row in output.splitlines() if motion.MOTION_SUMMARY in row][-1])
    if abs(census["held_leash_bound_deg"] - leash_bound) > 0.0005:
        raise ValueError("the runtime did not run with the lowered leash")
    if census["held_leash_ticks"] < 1:
        raise ValueError("the leash never turned a held view")
    if census["held_bound_handovers_unflagged"]:
        raise ValueError("a bound handover was not published as a cut")
    leashed = 0
    for epoch in epochs(output):
        for (tick, slot), row in sorted(probes(epoch).items()):
            if not row["valid"] or row["recovering"]:
                continue
            leashed += row["leash"] > 0
            if row["divergence"] > leash_bound + 1.0 and \
                    row["leash"] < LEASH_FULL_STEP - 1e-3:
                raise ValueError(f"tick {tick}: a held view {row['divergence']:.2f} degrees "
                                 f"out turned back only {row['leash']:.3f} (full rate "
                                 f"{LEASH_FULL_STEP:.3f})")
    if leashed == 0:
        raise ValueError("no held proof records a leash turn")
    return leashed


def verify(output, kind):
    if kind == "soft_decline":
        return verify_soft(output)
    if kind == "leash":
        return verify_leash(output)
    if kind in BOUND_ARMS:
        return verify_bound(output, kind)
    if kind not in ARMS:
        raise ValueError("unknown fault")
    everything = common(output)
    whole = output
    tick, slot, ticks = armed(output, ARMS[kind].split(":")[0])
    # Every tick-keyed check below reads the level the fault fired in.
    output = next(epoch for epoch in epochs(output)
                  if "camera_held_fault armed " in epoch)
    summaries = [row for row in output.splitlines()
                 if "camera_obstruction_observe summary" in row]
    rows = probes(output)
    active = published(output)
    before = rows.get((tick - 1, slot))
    if before is None or not before["valid"] or active.get((tick - 1, slot)) != 1:
        raise ValueError("no accepted, published held shot before the fault")
    fault = [row for row in summaries if f" tick={tick} " in row]
    if len(fault) != 1:
        raise ValueError("missing or ambiguous fault-tick summary")
    if kind == "projection":
        detail = details(output, "camera_obstruction_observe detail", tick, slot)
        if not re.search(r"resolve=\{status=4 valid=1 .* discontinuity=1 .* clearance=0 ", detail):
            raise ValueError("projection fault lacks validated fallback/cut")
        if rows.get((tick + 1, slot), {}).get("continued", 1) != 0:
            raise ValueError("chain not retired across failed projection")
    elif kind == "query":
        consumed = _rows(output, "camera_held_fault consumed ")
        if consumed != [f"camera_held_fault consumed kind=query tick={tick} slot={slot}"]:
            raise ValueError("the injection never reached the held chain's proof")
        row = rows.get((tick, slot))
        if row is None or row["valid"] or row["rejection"] in ("none", "history"):
            raise ValueError("held proof did not reject the unprovable candidate")
        marked = [row for row in everything if re.search(r"probe_degraded=[1-9]", row)]
        if marked != fault:
            raise ValueError("degraded held probe missing from, or repeated beyond, its tick")
        if any("target_hidden=0 " not in row for row in everything):
            raise ValueError("query fault lost target visibility")
    elif kind == "false_clear":
        if ticks != RELEASE_HOLD_TICKS - 1:
            raise ValueError("false-clear run is not one tick shorter than the hold")
        previous_detail = details(output, "camera_obstruction_observe detail", tick - 1, slot)
        previous_offset = _offset(previous_detail)
        previous_forward = vector(details(output, "camera_motion detail", tick - 1, slot), "forward")
        for held in range(tick, tick + ticks):
            row = rows.get((held, slot))
            if row is None or not row["valid"] or not row["continued"]:
                raise ValueError(f"held shot ended during the false-clear run at {held}")
            if row["recovering"] or row["contact"]:
                raise ValueError(f"false-clear tick {held} recovered instead of holding")
            if active.get((held, slot)) != 1:
                raise ValueError(f"no active held publication at {held}")
            detail = details(output, "camera_obstruction_observe detail", held, slot)
            if not re.search(r"resolve=\{status=1 valid=1 corrected=1 ", detail):
                raise ValueError(f"false-clear tick {held} not published as held/retracted")
            offset = _offset(detail)
            if max(abs(a - b) for a, b in zip(offset, previous_offset)) > RIGID_TOLERANCE:
                raise ValueError(f"held eye moved relative to its pivot at {held}: "
                                 f"{previous_offset} -> {offset}")
            sample = details(output, "camera_motion detail", held, slot)
            if vector(sample, "forward") != previous_forward:
                raise ValueError(f"held orientation moved at {held}")
            if not re.search(r"state=\{blocked=1 retracted=1 recovering=0 held=1 ", sample):
                raise ValueError(f"false-clear tick {held} not censused as a release-held tick")
            previous_offset = offset
    else:
        row = rows.get((tick, slot))
        if before["recovering"] != 1 or before["contact"] != 0:
            raise ValueError("false contact did not land inside a released recovery")
        if row is None or not row["valid"] or not row["contact"] or row["recovering"]:
            raise ValueError("held recovery did not re-hold on the injected contact")
        events = [event for event in motion.reengagement_evidence(output)
                  if f" tick={tick} " in event and f" physical_slot={slot} " in event]
        if len(events) != 1 or "prior_recovering=1 " not in events[0]:
            raise ValueError("the census did not count the held-recovery re-engagement")
        _summary, _stats, failures = motion.inspect(motion.ROUTES[0], whole)
        if not any("re-engagements" in failure for failure in failures):
            raise ValueError("held-shot chatter did not fail the motion gate")


def _offset(detail):
    effective = vector(detail, "effective")
    pivot = vector(detail, "pivot")
    return tuple(e - p for e, p in zip(effective, pivot))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=motion.DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--only", choices=(*ARMS, *BOUND_ARMS, "leash", "soft_decline"))
    args = parser.parse_args()
    binary = Path(motion.resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file() or not rom.is_file():
        parser.error("missing binary or ROM")
    for kind in (args.only,) if args.only else (*ARMS, *BOUND_ARMS, "leash", "soft_decline"):
        output = motion.run_route(arm_route(kind), binary, rom, None,
                                  present_env=arm_env(kind))
        verify(output, kind)
        print(f"PASS held-shot {kind}: located, observed and safely published", flush=True)
    return 0


def arm_route(kind):
    name = SOFT_ROUTE if kind == "soft_decline" else motion.ROUTES[0].name
    return next(route for route in motion.ROUTES if route.name == name)


def arm_env(kind):
    env = {"MDKR_CAMERA_TRACE": "2"}
    if kind == "soft_decline":
        env["MDKR_TEST_CAMERA_SOFT_STATIC"] = SOFT_STATIC_EXTENT
    elif kind == "leash":
        env["MDKR_TEST_CAMERA_HELD_BOUNDS"] = ":".join(str(value) for value in LEASH_ARM)
    elif kind in BOUND_ARMS:
        dwell, divergence = BOUND_ARMS[kind]
        env["MDKR_TEST_CAMERA_HELD_BOUNDS"] = f"{dwell}:{divergence}"
    else:
        env["MDKR_TEST_CAMERA_HELD_FAULT"] = ARMS[kind]
    return env


if __name__ == "__main__":
    raise SystemExit(main())
