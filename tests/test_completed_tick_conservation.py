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

# An arm that deliberately groups catch-up passes declares the debt its own
# rate implies; an arm whose debt is genuinely variable declares its window.
grouped = dict(clean, lead=2, maxpending=2)
require(completed_tick_conservation(grouped, 100, "grouped",
                                    expected_lead=2,
                                    expected_max_pending=2) is None,
        "declared two-tick catch-up debt was rejected")
require(completed_tick_conservation(grouped, 100, "windowed",
                                    expected_lead=(1, 30),
                                    expected_max_pending=(1, 30)) is None,
        "declared debt window was rejected")

for name, summary, expectation in (
        ("lost clock ticket", {"ticks": 100, "simticks": 100,
                               "issued": 99, "pending": 0, "updates": 99,
                               "lead": 1, "maxpending": 1}, {}),
        ("uncompleted final pass", {"ticks": 100, "simticks": 99,
                                    "issued": 99, "pending": 1, "updates": 98,
                                    "lead": 1, "maxpending": 1}, {}),
        ("extra issued ticket", {"ticks": 100, "simticks": 100,
                                 "issued": 100, "pending": 0, "updates": 99,
                                 "lead": 1, "maxpending": 1}, {}),
        ("hidden lead", {"ticks": 100, "simticks": 100,
                         "issued": 99, "pending": 1, "updates": 99,
                         "lead": 0, "maxpending": 1}, {}),
        # Why the debt marks are absolute instead of compared with each other:
        # a run that fell forty ticks behind and drained the backlog again is
        # entirely self-consistent, so lead==maxpending accepts it.
        ("forty-tick backlog", {"ticks": 100, "simticks": 100,
                                "issued": 99, "pending": 1, "updates": 99,
                                "lead": 40, "maxpending": 40}, {}),
        ("backlog above a declared window", {"ticks": 100, "simticks": 100,
                                             "issued": 99, "pending": 1,
                                             "updates": 99, "lead": 40,
                                             "maxpending": 40},
         {"expected_lead": (1, 30), "expected_max_pending": (1, 30)}),
        ("undeclared grouped catch-up", {"ticks": 100, "simticks": 100,
                                         "issued": 99, "pending": 1,
                                         "updates": 99, "lead": 2,
                                         "maxpending": 2}, {}),
        ("debt high-water without a sampled lead",
         {"ticks": 100, "simticks": 100, "issued": 99, "pending": 1,
          "updates": 99, "lead": 1, "maxpending": 40}, {}),
):
    require(completed_tick_conservation(summary, 100, name, **expectation)
            is not None,
            f"{name} control was accepted")

print("completed tick conservation: PASS")
