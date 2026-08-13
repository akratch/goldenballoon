#!/usr/bin/env python3
"""Validate seven contiguous privacy-safe $0 beta evidence days."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import date, timedelta
import json
from pathlib import Path
import re
from typing import Any, NoReturn

from reconcile_party_usage import ReconciliationError, reconcile


MAX_LEDGER_BYTES = 512 * 1024
OPERATIONS = {
    "matchCreate": ("pairing", 10),
    "matchLinkJoin": ("pairing", 2),
    "matchCodeJoin": ("pairing", 3),
    "matchControl": ("control", 2),
    "matchRotate": ("control", 10),
    "matchSocket": ("control", 4),
    "matchSignalSocket": ("control", 15),
    "partyCreate": ("pairing", 10),
    "partyLinkJoin": ("pairing", 2),
    "partyCodeJoin": ("pairing", 3),
    "partyControl": ("control", 2),
    "partyRotate": ("control", 10),
    "partySocket": ("control", 28),
}
RESERVATION_KEYS = frozenset({*OPERATIONS, "legacy"})


class LedgerError(Exception):
    """A bounded failure code that never contains ledger-derived text."""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


@dataclass(frozen=True)
class LedgerResult:
    start_day: str
    end_day: str
    watch_days: int


def fail(code: str) -> NoReturn:
    raise LedgerError(code)


def boolean(value: Any, code: str) -> bool:
    if not isinstance(value, bool):
        fail(code)
    return value


def exact_object(value: Any, keys: frozenset[str], code: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        fail(code)
    return value


def integer(value: Any, minimum: int, maximum: int, code: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        fail(code)
    if value < minimum or value > maximum:
        fail(code)
    return value


def utc_day(value: Any, code: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
        fail(code)
    try:
        parsed = date.fromisoformat(value)
    except ValueError:
        fail(code)
    if parsed.isoformat() != value:
        fail(code)
    return value


def digest(value: Any, lengths: tuple[int, ...], code: str) -> str:
    if not isinstance(value, str) or len(value) not in lengths or not re.fullmatch(
            r"[0-9a-f]+", value):
        fail(code)
    return value


def parse_health(value: Any, expected_day: str,
                 pairing_units: int, control_units: int) -> None:
    root = exact_object(value, frozenset({
        "schemaVersion", "reservationRequests", "reservations", "admitted",
        "tracked", "tracking", "day",
    }), "health_schema")
    integer(root["schemaVersion"], 2, 2, "health_version")
    if utc_day(root["day"], "health_day") != expected_day:
        fail("health_day_mismatch")
    reservations = exact_object(root["reservations"], RESERVATION_KEYS,
                                "health_reservations_schema")
    parsed: dict[str, int] = {}
    for operation in sorted(RESERVATION_KEYS):
        parsed[operation] = integer(reservations[operation], 0, 19_900,
                                    "health_reservation_value")
    if integer(root["reservationRequests"], 0, 19_900,
               "health_request_count") != sum(parsed.values()):
        fail("health_request_invariant")
    admitted = exact_object(root["admitted"],
                            frozenset({"pairingUnits", "controlUnits"}),
                            "health_admitted_schema")
    admitted_pairing = integer(admitted["pairingUnits"], 0, 12_000,
                               "health_admitted_pairing")
    admitted_control = integer(admitted["controlUnits"], 0, 19_900,
                               "health_admitted_control")
    if admitted_pairing != pairing_units or admitted_control != control_units:
        fail("health_capacity_mismatch")
    tracked = exact_object(root["tracked"],
                           frozenset({"pairingUnits", "controlUnits"}),
                           "health_tracked_schema")
    tracked_pairing = integer(tracked["pairingUnits"], 0, 1_000_000,
                              "health_tracked_pairing")
    tracked_control = integer(tracked["controlUnits"], 0, 1_000_000,
                              "health_tracked_control")
    expected_pairing = sum(parsed[name] * units for name, (kind, units) in
                           OPERATIONS.items() if kind == "pairing")
    expected_control = sum(parsed[name] * units for name, (kind, units) in
                           OPERATIONS.items() if kind == "control")
    if tracked_pairing != expected_pairing or tracked_control != expected_control:
        fail("health_tracking_invariant")
    if root["tracking"] != "complete" or parsed["legacy"] != 0:
        fail("health_tracking_incomplete")
    if tracked_pairing != admitted_pairing or tracked_control != admitted_control:
        fail("health_tracking_mismatch")


def parse_experience(value: Any) -> None:
    root = exact_object(value, frozenset({
        "schemaVersion", "source", "matchCreate", "matchJoin",
        "phoneDirect", "decision",
    }), "experience_schema")
    integer(root["schemaVersion"], 1, 1, "experience_version")
    if root["source"] != "synthetic_canary_v1":
        fail("experience_source")
    if root["decision"] != "GO":
        fail("experience_decision")

    def request_metric(raw: Any, prefix: str) -> None:
        metric = exact_object(raw, frozenset({"attempts", "successes", "p95Ms"}),
                              f"{prefix}_schema")
        attempts = integer(metric["attempts"], 20, 20, f"{prefix}_attempts")
        successes = integer(metric["successes"], 0, attempts,
                            f"{prefix}_successes")
        if successes < 19:
            fail(f"{prefix}_success_rate")
        integer(metric["p95Ms"], 0, 2_500, f"{prefix}_p95")

    request_metric(root["matchCreate"], "match_create")
    request_metric(root["matchJoin"], "match_join")
    direct = exact_object(root["phoneDirect"], frozenset({
        "attempts", "successes", "setupP95Ms", "inputRttP95Ms",
    }), "phone_direct_schema")
    attempts = integer(direct["attempts"], 20, 20, "phone_direct_attempts")
    successes = integer(direct["successes"], 0, attempts,
                        "phone_direct_successes")
    if successes < 18:
        fail("phone_direct_success_rate")
    integer(direct["setupP95Ms"], 0, 8_000, "phone_direct_setup_p95")
    integer(direct["inputRttP95Ms"], 0, 250, "phone_direct_input_rtt_p95")


def validate(value: Any) -> LedgerResult:
    root = exact_object(value, frozenset({
        "schemaVersion", "window", "days", "finalDecision",
    }), "ledger_schema")
    integer(root["schemaVersion"], 3, 3, "ledger_version")
    window = exact_object(root["window"],
                          frozenset({"startDay", "endDay", "timeZone"}),
                          "window_schema")
    start_day = utc_day(window["startDay"], "window_start")
    end_day = utc_day(window["endDay"], "window_end")
    if window["timeZone"] != "UTC":
        fail("window_timezone")
    if root["finalDecision"] != "GO":
        fail("final_decision")
    days = root["days"]
    if not isinstance(days, list) or len(days) != 7:
        fail("day_count")
    expected = date.fromisoformat(start_day)
    watch_days = 0
    for index, raw_day in enumerate(days):
        entry = exact_object(raw_day, frozenset({
            "day", "commit", "providerDeploymentDigest", "internal",
            "provider", "health", "experience", "localPlayAvailable", "incidentOpen",
            "decision",
        }), "day_schema")
        day = utc_day(entry["day"], "entry_day")
        if day != (expected + timedelta(days=index)).isoformat():
            fail("day_sequence")
        digest(entry["commit"], (40, 64), "commit_digest")
        digest(entry["providerDeploymentDigest"], (64,),
               "provider_deployment_digest")
        if not boolean(entry["localPlayAvailable"], "local_play_probe"):
            fail("local_play_probe")
        if boolean(entry["incidentOpen"], "incident_state"):
            fail("incident_state")
        if entry["decision"] != "GO":
            fail("daily_decision")
        try:
            result = reconcile(entry["internal"], entry["provider"])
        except ReconciliationError:
            fail("daily_reconciliation")
        if result.day != day:
            fail("entry_reconciliation_day")
        parse_health(entry["health"], day, result.pairing_units,
                     result.control_units)
        parse_experience(entry["experience"])
        if result.level == "watch":
            watch_days += 1
    expected_end = (expected + timedelta(days=6)).isoformat()
    if end_day != expected_end:
        fail("window_range")
    return LedgerResult(start_day=start_day, end_day=end_day,
                        watch_days=watch_days)


def read_json(path: Path) -> Any:
    try:
        if path.stat().st_size > MAX_LEDGER_BYTES:
            fail("input_too_large")
        with path.open("r", encoding="utf-8") as handle:
            raw = handle.read(MAX_LEDGER_BYTES + 1)
        if len(raw.encode("utf-8")) > MAX_LEDGER_BYTES:
            fail("input_too_large")
        return json.loads(raw)
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError):
        fail("input_unavailable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ledger", type=Path, required=True,
                        help="privacy-safe seven-day beta ledger JSON")
    args = parser.parse_args()
    try:
        result = validate(read_json(args.ledger))
    except LedgerError as error:
        print(f"validate_party_beta_ledger: STOP code={error.code}")
        return 1
    print("validate_party_beta_ledger: PASS "
          f"startDay={result.start_day} endDay={result.end_day} days=7 "
          f"watchDays={result.watch_days} zeroChargeDays=7 localPlayDays=7 "
          "tracking=complete experienceDays=7 incidents=0 decision=GO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
