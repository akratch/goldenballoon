#!/usr/bin/env python3
"""Reconcile privacy-safe Party capacity and provider usage aggregates.

The inputs are deliberately small, exact schemas. Unknown fields are rejected
instead of copied into diagnostics, so this tool cannot quietly turn an
operations artifact into a room, credential, address, or player ledger.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import date
import json
from pathlib import Path
import re
from typing import Any, NoReturn


INTERNAL_TOP_KEYS = frozenset({
    "schemaVersion", "day", "admitted", "refusalObserved", "remaining",
    "admissionPercent", "level",
})
PROVIDER_TOP_KEYS = frozenset({
    "schemaVersion", "day", "plan", "billing", "usage", "limits",
})
USAGE_KEYS = frozenset({
    "workerRequests", "durableObjectRequests",
    "durableObjectDurationGbSeconds", "rowsRead", "rowsWritten", "storageBytes",
})

# Reviewed 2026-08-12 Free-plan ceilings. A provider report may lower these
# values; a higher value needs a source review and code change rather than an
# optimistic runtime override.
REVIEWED_FREE_CEILINGS = {
    "workerRequests": 100_000,
    "durableObjectRequests": 100_000,
    "durableObjectDurationGbSeconds": 13_000,
    "rowsRead": 5_000_000,
    "rowsWritten": 100_000,
    "storageBytes": 5_000_000_000,
}
MAX_INPUT_BYTES = 64 * 1024


class ReconciliationError(Exception):
    """A bounded operator-facing failure code with no input-derived text."""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


@dataclass(frozen=True)
class ReconciliationResult:
    day: str
    level: str
    pairing_units: int
    control_units: int
    provider_max_basis_points: int
    provider_dimension: str


def fail(code: str) -> NoReturn:
    raise ReconciliationError(code)


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


def boolean(value: Any, code: str) -> bool:
    if not isinstance(value, bool):
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


def parse_internal(value: Any) -> dict[str, Any]:
    root = exact_object(value, INTERNAL_TOP_KEYS, "internal_schema")
    if integer(root["schemaVersion"], 1, 1, "internal_version") != 1:
        fail("internal_version")
    day = utc_day(root["day"], "internal_day")
    admitted = exact_object(root["admitted"],
                            frozenset({"pairingUnits", "controlUnits"}),
                            "internal_admitted_schema")
    pairing = integer(admitted["pairingUnits"], 0, 12_000,
                      "internal_pairing_units")
    control = integer(admitted["controlUnits"], 0, 20_000,
                      "internal_control_units")
    if pairing + control > 19_900:
        fail("internal_total_units")
    refusal = exact_object(root["refusalObserved"],
                           frozenset({"pairing", "control"}),
                           "internal_refusal_schema")
    pairing_refusal = boolean(refusal["pairing"], "internal_pairing_refusal")
    control_refusal = boolean(refusal["control"], "internal_control_refusal")
    remaining = exact_object(root["remaining"],
                             frozenset({"admissionUnits", "controlUnits"}),
                             "internal_remaining_schema")
    remaining_admission = integer(remaining["admissionUnits"], 0, 12_000,
                                  "internal_remaining_admission")
    remaining_control = integer(remaining["controlUnits"], 0, 19_900,
                                "internal_remaining_control")
    if remaining_control != max(0, 19_900 - pairing - control):
        fail("internal_control_invariant")
    percent = integer(root["admissionPercent"], 0, 100,
                      "internal_admission_percent")
    derived_admission_ceiling = pairing + remaining_admission
    if derived_admission_ceiling > 12_000:
        fail("internal_admission_ceiling")
    if (remaining_admission > 0 and
            derived_admission_ceiling > max(0, 18_000 - control)):
        fail("internal_reserve_invariant")
    expected_percent = (100 if derived_admission_ceiling == 0 else
                        min(100, pairing * 100 // derived_admission_ceiling))
    if percent != expected_percent:
        fail("internal_admission_invariant")
    expected_level = ("closed" if percent >= 90 else
                      "freeze" if percent >= 75 else
                      "watch" if percent >= 50 else "normal")
    if root["level"] != expected_level:
        fail("internal_level")
    return {
        "day": day,
        "pairing": pairing,
        "control": control,
        "pairing_refusal": pairing_refusal,
        "control_refusal": control_refusal,
        "level": expected_level,
    }


def parse_provider(value: Any) -> dict[str, Any]:
    root = exact_object(value, PROVIDER_TOP_KEYS, "provider_schema")
    if integer(root["schemaVersion"], 2, 2, "provider_version") != 2:
        fail("provider_version")
    day = utc_day(root["day"], "provider_day")
    if root["plan"] != "free":
        fail("provider_paid_plan")
    billing = exact_object(root["billing"], frozenset({
        "methodAttached", "paidOveragesEnabled", "currencyCode",
        "chargeMicros",
    }), "provider_billing_schema")
    if boolean(billing["methodAttached"], "provider_billing_method"):
        fail("provider_billing_method")
    if boolean(billing["paidOveragesEnabled"], "provider_paid_overages"):
        fail("provider_paid_overages")
    currency = billing["currencyCode"]
    if not isinstance(currency, str) or not re.fullmatch(r"[A-Z]{3}", currency):
        fail("provider_currency")
    if integer(billing["chargeMicros"], 0, 10**15,
               "provider_charge") != 0:
        fail("provider_charge")

    usage = exact_object(root["usage"], USAGE_KEYS, "provider_usage_schema")
    limits = exact_object(root["limits"], USAGE_KEYS, "provider_limits_schema")
    parsed_usage: dict[str, int] = {}
    parsed_limits: dict[str, int] = {}
    max_basis_points = 0
    max_dimension = "workerRequests"
    for key in sorted(USAGE_KEYS):
        reviewed = REVIEWED_FREE_CEILINGS[key]
        limit = integer(limits[key], 1, reviewed, "unreviewed_provider_limit")
        used = integer(usage[key], 0, reviewed, "provider_usage_value")
        if used > limit:
            fail("provider_limit_exceeded")
        parsed_limits[key] = limit
        parsed_usage[key] = used
        basis_points = (used * 10_000 + limit - 1) // limit
        if basis_points > max_basis_points:
            max_basis_points = basis_points
            max_dimension = key
    return {
        "day": day,
        "usage": parsed_usage,
        "limits": parsed_limits,
        "max_basis_points": max_basis_points,
        "max_dimension": max_dimension,
    }


def reconcile(internal_value: Any, provider_value: Any) -> ReconciliationResult:
    internal = parse_internal(internal_value)
    provider = parse_provider(provider_value)
    if internal["day"] != provider["day"]:
        fail("day_mismatch")
    if internal["control_refusal"]:
        fail("control_refusal")
    if internal["pairing_refusal"]:
        fail("pairing_refusal")
    if internal["level"] in {"freeze", "closed"}:
        fail("internal_stop_line")
    if provider["max_basis_points"] >= 7_500:
        fail("provider_stop_line")
    if internal["pairing"] + internal["control"] > 0 and (
            provider["usage"]["workerRequests"] == 0 or
            provider["usage"]["durableObjectRequests"] == 0):
        fail("provider_activity_mismatch")
    level = "watch" if (internal["level"] == "watch" or
                         provider["max_basis_points"] >= 5_000) else "normal"
    return ReconciliationResult(
        day=internal["day"], level=level,
        pairing_units=internal["pairing"],
        control_units=internal["control"],
        provider_max_basis_points=provider["max_basis_points"],
        provider_dimension=provider["max_dimension"],
    )


def read_json(path: Path) -> Any:
    try:
        if path.stat().st_size > MAX_INPUT_BYTES:
            fail("input_too_large")
        with path.open("r", encoding="utf-8") as handle:
            raw = handle.read(MAX_INPUT_BYTES + 1)
        if len(raw.encode("utf-8")) > MAX_INPUT_BYTES:
            fail("input_too_large")
        return json.loads(raw)
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError):
        fail("input_unavailable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--internal", type=Path, required=True,
                        help="saved /api/ops/capacity aggregate")
    parser.add_argument("--provider", type=Path, required=True,
                        help="provider aggregate exported by approved tooling")
    args = parser.parse_args()
    try:
        result = reconcile(read_json(args.internal), read_json(args.provider))
    except ReconciliationError as error:
        print(f"reconcile_party_usage: STOP code={error.code}")
        return 1
    print(
        "reconcile_party_usage: PASS "
        f"day={result.day} level={result.level} "
        f"pairingUnits={result.pairing_units} "
        f"controlUnits={result.control_units} "
        f"providerMaxBasisPoints={result.provider_max_basis_points} "
        f"providerDimension={result.provider_dimension} "
        "zeroCharge=1 billing=off"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
