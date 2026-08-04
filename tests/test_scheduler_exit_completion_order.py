#!/usr/bin/env python3
"""Guard the osRecvMesg boundary: complete this pass before gating the next."""

from pathlib import Path
import sys


source = (Path(__file__).resolve().parents[1] / "platform" /
          "stubs_dkr.c").read_text(encoding="utf-8")
boundary = source[source.index(
    "/* osRecvMesg is reached after fb_update has completed the current"):
    source.index("s_viFieldsPending = mdkr_pacing_queue_refill", source.index(
        "/* osRecvMesg is reached after fb_update has completed the current"))]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)


completion = boundary.index("g_simTickCounter++;")
headless = boundary.index("platform_headless_tick_complete")
trace = boundary.index("present_sched_trace_entry")
audio_advance = boundary.index("dkr_audio_advance_fields")
audio_service = boundary.index("dkr_audio_service_tick();")
exit_sample = boundary.index("const bool exit_requested")
input_commit = boundary.index("platform_input_commit_tick")
next_gate = boundary.index("mdkr_next_tick_dispatch_allowed")

require(completion < headless < trace < audio_advance < audio_service <
        exit_sample < input_commit < next_gate,
        "completed-pass accounting/audio must precede exit and next-ticket gate")
require("if (!exit_requested && oracle_variable_ticket)" in boundary and
        "else if (!exit_requested && present_sched_take_tick())" in boundary,
        "exit must prevent input commit and ticket consumption for the next pass")

print("scheduler exit completion order: PASS")
