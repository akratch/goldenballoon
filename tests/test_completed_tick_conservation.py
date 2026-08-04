#!/usr/bin/env python3
"""Positive and negative controls for finite scheduler-summary accounting."""

from harness_utils import completed_tick_conservation


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


clean = {"ticks": 100, "simticks": 100, "issued": 99, "pending": 1,
         "updates": 99, "lead": 1, "maxpending": 1}
require(completed_tick_conservation(clean, 100, "clean") is None,
        "completed pass plus one withheld next ticket was rejected")

for name, summary in (
        ("lost clock ticket", {"ticks": 100, "simticks": 100,
                               "issued": 99, "pending": 0, "updates": 99,
                               "lead": 1, "maxpending": 1}),
        ("uncompleted final pass", {"ticks": 100, "simticks": 99,
                                    "issued": 99, "pending": 1, "updates": 98,
                                    "lead": 1, "maxpending": 1}),
        ("extra issued ticket", {"ticks": 100, "simticks": 100,
                                 "issued": 100, "pending": 0, "updates": 99,
                                 "lead": 1, "maxpending": 1}),
        ("hidden lead", {"ticks": 100, "simticks": 100,
                         "issued": 99, "pending": 1, "updates": 99,
                         "lead": 0, "maxpending": 1}),
):
    require(completed_tick_conservation(summary, 100, name) is not None,
            f"{name} control was accepted")

print("completed tick conservation: PASS")
