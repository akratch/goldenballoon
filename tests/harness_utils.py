"""Shared, side-effect-free helpers for the headless regression checks."""

from __future__ import annotations

import os
from collections.abc import Mapping
from pathlib import Path


def resolve_binary(build: str | os.PathLike[str]) -> str:
    """Resolve ``--build`` consistently from either a directory or executable.

    Every behavioural check accepts the same contract:

    - ``--build build-rel`` resolves to ``build-rel/mdkr64``;
    - ``--build build-rel/mdkr64`` remains that executable.

    Existence is intentionally checked by each caller so its existing failure
    message and any additional input validation remain intact.
    """

    path = Path(build).expanduser()
    if path.is_dir():
        path = path / "mdkr64"
    return str(path)


def completed_tick_conservation(summary: Mapping[str, int], expected_ticks: int,
                                label: str) -> str | None:
    """Validate a finite run after its final pass, before its next ticket.

    The scheduler records the pass that reached the headless budget and only
    then prevents dispatch of a *next* pass. Consequently one already-earned
    host ticket remains pending at clean termination. This is distinct from a
    live-frame debt assertion: callers keep their own intermediate pacing
    checks, while this helper is only for the terminal summary.
    """
    ticks = summary.get("ticks", -1)
    simticks = summary.get("simticks", -1)
    issued = summary.get("issued", -1)
    pending = summary.get("pending", -1)
    updates = summary.get("updates", -1)
    lead = summary.get("lead", -1)
    max_pending = summary.get("maxpending", -1)
    if ticks != expected_ticks or simticks != expected_ticks:
        return (f"{label}: completed ticks/simticks={ticks}/{simticks}, "
                f"expected {expected_ticks}/{expected_ticks}")
    if issued + pending != ticks:
        return (f"{label}: clock conservation failed: ticks={ticks}, "
                f"issued={issued}, pending={pending}")
    if simticks != issued + 1:
        return (f"{label}: bootstrap completion failed: simticks={simticks}, "
                f"issued={issued}")
    if updates + 1 != simticks:
        return (f"{label}: game-update completion failed: updates={updates}, "
                f"simticks={simticks}")
    if pending != 1:
        return (f"{label}: clean completion must retain exactly one next "
                f"ticket, pending={pending}")
    # present_sched_trace_entry samples clock-minus-issued before taking the
    # next ticket, so its high-water lead is exactly the driver's pending-debt
    # high water. A clean steady run therefore ends with lead=maxpending=1;
    # grouped catch-up may raise both, but they must never disagree.
    if lead != max_pending:
        return (f"{label}: scheduler lead/maxpending={lead}/{max_pending}, "
                "expected exact agreement")
    return None
