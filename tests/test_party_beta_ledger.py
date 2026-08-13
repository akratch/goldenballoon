#!/usr/bin/env python3
"""Adversarial tests for the seven-day $0 beta evidence ledger."""

from __future__ import annotations

import copy
from datetime import date, timedelta
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
TOOL = TOOLS / "validate_party_beta_ledger.py"
SPEC = importlib.util.spec_from_file_location("validate_party_beta_ledger", TOOL)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def internal(day: str) -> dict[str, object]:
    return {
        "schemaVersion": 1, "day": day,
        "admitted": {"pairingUnits": 120, "controlUnits": 42},
        "refusalObserved": {"pairing": False, "control": False},
        "remaining": {"admissionUnits": 9_880, "controlUnits": 19_738},
        "admissionPercent": 1, "level": "normal",
    }


def provider(day: str) -> dict[str, object]:
    return {
        "schemaVersion": 2, "day": day, "plan": "free",
        "billing": {"methodAttached": False, "paidOveragesEnabled": False,
                    "currencyCode": "USD", "chargeMicros": 0},
        "usage": {"workerRequests": 500, "durableObjectRequests": 450,
                  "durableObjectDurationGbSeconds": 10,
                  "rowsRead": 1_000, "rowsWritten": 250,
                  "storageBytes": 65_536},
        "limits": {"workerRequests": 100_000,
                   "durableObjectRequests": 100_000,
                   "durableObjectDurationGbSeconds": 13_000,
                   "rowsRead": 5_000_000, "rowsWritten": 100_000,
                   "storageBytes": 5_000_000_000},
    }


def health(day: str) -> dict[str, object]:
    reservations = {
        "matchCreate": 0, "matchLinkJoin": 0, "matchCodeJoin": 0,
        "matchControl": 21, "matchRotate": 0, "matchSocket": 0,
        "matchSignalSocket": 0,
        "partyCreate": 12, "partyLinkJoin": 0, "partyCodeJoin": 0,
        "partyControl": 0, "partyRotate": 0, "partySocket": 0, "legacy": 0,
    }
    return {
        "schemaVersion": 2, "day": day, "reservationRequests": 33,
        "reservations": reservations,
        "admitted": {"pairingUnits": 120, "controlUnits": 42},
        "tracked": {"pairingUnits": 120, "controlUnits": 42},
        "tracking": "complete",
    }


def experience() -> dict[str, object]:
    return {
        "schemaVersion": 1, "source": "synthetic_canary_v1",
        "matchCreate": {"attempts": 20, "successes": 20, "p95Ms": 900},
        "matchJoin": {"attempts": 20, "successes": 19, "p95Ms": 1_100},
        "phoneDirect": {"attempts": 20, "successes": 19,
                        "setupP95Ms": 3_500, "inputRttP95Ms": 90},
        "decision": "GO",
    }


def ledger() -> dict[str, object]:
    start = date(2026, 8, 1)
    days = []
    for offset in range(7):
        day = (start + timedelta(days=offset)).isoformat()
        days.append({
            "day": day, "commit": "a" * 40,
            "providerDeploymentDigest": "b" * 64,
            "internal": internal(day), "provider": provider(day),
            "health": health(day), "experience": experience(),
            "localPlayAvailable": True,
            "incidentOpen": False, "decision": "GO",
        })
    return {"schemaVersion": 3,
            "window": {"startDay": "2026-08-01", "endDay": "2026-08-07",
                       "timeZone": "UTC"},
            "days": days, "finalDecision": "GO"}


def expect_error(code: str, value: dict[str, object]) -> None:
    try:
        MODULE.validate(value)
    except MODULE.LedgerError as error:
        assert error.code == code, (error.code, code)
    else:
        raise AssertionError(f"expected {code}")


def main() -> int:
    result = MODULE.validate(ledger())
    assert result.start_day == "2026-08-01"
    assert result.end_day == "2026-08-07"
    assert result.watch_days == 0

    cases: list[tuple[str, dict[str, object]]] = []
    changed = ledger(); changed["private"] = "canary"
    cases.append(("ledger_schema", changed))
    changed = ledger(); changed["schemaVersion"] = 2
    cases.append(("ledger_version", changed))
    changed = ledger(); changed["days"] = changed["days"][:6]  # type: ignore[index]
    cases.append(("day_count", changed))
    changed = ledger(); changed["days"][3]["day"] = "2026-08-05"  # type: ignore[index]
    cases.append(("day_sequence", changed))
    changed = ledger(); changed["window"]["endDay"] = "2026-08-08"  # type: ignore[index]
    cases.append(("window_range", changed))
    changed = ledger(); changed["days"][0]["commit"] = "not-a-digest"  # type: ignore[index]
    cases.append(("commit_digest", changed))
    changed = ledger(); changed["days"][0]["localPlayAvailable"] = False  # type: ignore[index]
    cases.append(("local_play_probe", changed))
    changed = ledger(); changed["days"][0]["incidentOpen"] = True  # type: ignore[index]
    cases.append(("incident_state", changed))
    changed = ledger(); changed["days"][0]["decision"] = "STOP"  # type: ignore[index]
    cases.append(("daily_decision", changed))
    changed = ledger(); changed["days"][0]["provider"]["billing"]["chargeMicros"] = 1  # type: ignore[index]
    cases.append(("daily_reconciliation", changed))
    changed = ledger(); changed["days"][0]["provider"]["usage"]["durableObjectDurationGbSeconds"] = 9_750  # type: ignore[index]
    cases.append(("daily_reconciliation", changed))
    changed = ledger(); changed["days"][0]["health"]["tracking"] = "partial"  # type: ignore[index]
    cases.append(("health_tracking_incomplete", changed))
    changed = ledger(); changed["days"][0]["health"]["reservations"]["legacy"] = 1  # type: ignore[index]
    changed["days"][0]["health"]["reservationRequests"] = 34  # type: ignore[index]
    cases.append(("health_tracking_incomplete", changed))
    changed = ledger(); changed["days"][0]["health"]["tracked"]["controlUnits"] = 44  # type: ignore[index]
    cases.append(("health_tracking_invariant", changed))
    changed = ledger(); changed["days"][0]["health"]["admitted"]["controlUnits"] = 44  # type: ignore[index]
    cases.append(("health_capacity_mismatch", changed))
    changed = ledger(); changed["days"][0]["health"]["reservations"]["matchControl"] = True  # type: ignore[index]
    cases.append(("health_reservation_value", changed))
    changed = ledger(); changed["days"][0]["experience"]["private"] = "canary"  # type: ignore[index]
    cases.append(("experience_schema", changed))
    changed = ledger(); changed["days"][0]["experience"]["source"] = "user_analytics"  # type: ignore[index]
    cases.append(("experience_source", changed))
    changed = ledger(); changed["days"][0]["experience"]["matchCreate"]["attempts"] = True  # type: ignore[index]
    cases.append(("match_create_attempts", changed))
    changed = ledger(); changed["days"][0]["experience"]["matchCreate"]["successes"] = 18  # type: ignore[index]
    cases.append(("match_create_success_rate", changed))
    changed = ledger(); changed["days"][0]["experience"]["matchCreate"]["p95Ms"] = 2_501  # type: ignore[index]
    cases.append(("match_create_p95", changed))
    changed = ledger(); changed["days"][0]["experience"]["matchJoin"]["successes"] = 18  # type: ignore[index]
    cases.append(("match_join_success_rate", changed))
    changed = ledger(); changed["days"][0]["experience"]["phoneDirect"]["successes"] = 17  # type: ignore[index]
    cases.append(("phone_direct_success_rate", changed))
    changed = ledger(); changed["days"][0]["experience"]["phoneDirect"]["setupP95Ms"] = 8_001  # type: ignore[index]
    cases.append(("phone_direct_setup_p95", changed))
    changed = ledger(); changed["days"][0]["experience"]["phoneDirect"]["inputRttP95Ms"] = 251  # type: ignore[index]
    cases.append(("phone_direct_input_rtt_p95", changed))
    changed = ledger(); changed["days"][0]["experience"]["decision"] = "STOP"  # type: ignore[index]
    cases.append(("experience_decision", changed))
    for code, value in cases:
        expect_error(code, value)

    with tempfile.TemporaryDirectory(prefix="mdkr-beta-ledger-") as temporary:
        path = Path(temporary) / "ledger.json"
        path.write_text(json.dumps(ledger()), encoding="utf-8")
        passed = subprocess.run([sys.executable, str(TOOL), "--ledger", str(path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False)
        assert passed.returncode == 0 and "days=7" in passed.stdout
        poisoned = copy.deepcopy(ledger())
        poisoned["credential"] = "PRIVATE-CANARY-DO-NOT-ECHO"
        path.write_text(json.dumps(poisoned), encoding="utf-8")
        stopped = subprocess.run([sys.executable, str(TOOL), "--ledger", str(path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False)
        assert stopped.returncode == 1
        assert stopped.stdout.strip() == \
            "validate_party_beta_ledger: STOP code=ledger_schema"
        assert "PRIVATE-CANARY" not in stopped.stdout
        path.write_bytes(b" " * (MODULE.MAX_LEDGER_BYTES + 1))
        oversized = subprocess.run([sys.executable, str(TOOL), "--ledger", str(path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False)
        assert oversized.returncode == 1
        assert oversized.stdout.strip() == \
            "validate_party_beta_ledger: STOP code=input_too_large"

    print("test_party_beta_ledger: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
