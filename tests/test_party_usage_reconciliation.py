#!/usr/bin/env python3
"""Adversarial contract tests for zero-cost provider reconciliation."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools/reconcile_party_usage.py"
SPEC = importlib.util.spec_from_file_location("reconcile_party_usage", TOOL)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def internal() -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "day": "2026-08-12",
        "admitted": {"pairingUnits": 120, "controlUnits": 42},
        "refusalObserved": {"pairing": False, "control": False},
        "remaining": {"admissionUnits": 9_880, "controlUnits": 19_738},
        "admissionPercent": 1,
        "level": "normal",
    }


def provider() -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "day": "2026-08-12",
        "plan": "free",
        "billing": {
            "methodAttached": False,
            "paidOveragesEnabled": False,
            "currencyCode": "USD",
            "chargeMicros": 0,
        },
        "usage": {
            "workerRequests": 500,
            "durableObjectRequests": 450,
            "rowsRead": 1_000,
            "rowsWritten": 250,
            "storageBytes": 65_536,
        },
        "limits": {
            "workerRequests": 100_000,
            "durableObjectRequests": 100_000,
            "rowsRead": 5_000_000,
            "rowsWritten": 100_000,
            "storageBytes": 5_000_000_000,
        },
    }


def expect_error(code: str, left: dict[str, object],
                 right: dict[str, object]) -> None:
    try:
        MODULE.reconcile(left, right)
    except MODULE.ReconciliationError as error:
        assert error.code == code, (error.code, code)
    else:
        raise AssertionError(f"expected {code}")


def main() -> int:
    result = MODULE.reconcile(internal(), provider())
    assert result.level == "normal"
    assert result.provider_dimension == "workerRequests"
    assert result.provider_max_basis_points == 50

    watched = provider()
    watched["usage"]["workerRequests"] = 50_000  # type: ignore[index]
    assert MODULE.reconcile(internal(), watched).level == "watch"

    cases: list[tuple[str, dict[str, object], dict[str, object]]] = []
    changed_internal = internal()
    changed_internal["roomId"] = "must-never-appear"
    cases.append(("internal_schema", changed_internal, provider()))
    changed_provider = provider()
    changed_provider["accountId"] = "must-never-appear"
    cases.append(("provider_schema", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["billing"]["methodAttached"] = True  # type: ignore[index]
    cases.append(("provider_billing_method", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["billing"]["paidOveragesEnabled"] = True  # type: ignore[index]
    cases.append(("provider_paid_overages", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["billing"]["chargeMicros"] = 1  # type: ignore[index]
    cases.append(("provider_charge", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["limits"]["workerRequests"] = 100_001  # type: ignore[index]
    cases.append(("unreviewed_provider_limit", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["usage"]["workerRequests"] = 90_000  # type: ignore[index]
    cases.append(("provider_stop_line", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["usage"]["workerRequests"] = 0  # type: ignore[index]
    cases.append(("provider_activity_mismatch", internal(), changed_provider))
    changed_internal = internal()
    changed_internal["refusalObserved"]["pairing"] = True  # type: ignore[index]
    cases.append(("pairing_refusal", changed_internal, provider()))
    changed_internal = internal()
    changed_internal["refusalObserved"]["control"] = True  # type: ignore[index]
    cases.append(("control_refusal", changed_internal, provider()))
    changed_internal = internal()
    changed_internal["admitted"]["pairingUnits"] = 7_500  # type: ignore[index]
    changed_internal["remaining"]["admissionUnits"] = 2_500  # type: ignore[index]
    changed_internal["remaining"]["controlUnits"] = 12_358  # type: ignore[index]
    changed_internal["admissionPercent"] = 75
    changed_internal["level"] = "freeze"
    cases.append(("internal_stop_line", changed_internal, provider()))
    changed_internal = internal()
    changed_internal["remaining"]["controlUnits"] = 19_737  # type: ignore[index]
    cases.append(("internal_control_invariant", changed_internal, provider()))
    changed_internal = internal()
    changed_internal["admissionPercent"] = 2
    changed_internal["level"] = "normal"
    cases.append(("internal_admission_invariant", changed_internal, provider()))
    changed_internal = internal()
    changed_internal["admitted"]["pairingUnits"] = 12_000  # type: ignore[index]
    changed_internal["admitted"]["controlUnits"] = 7_901  # type: ignore[index]
    changed_internal["remaining"]["admissionUnits"] = 0  # type: ignore[index]
    changed_internal["remaining"]["controlUnits"] = 0  # type: ignore[index]
    changed_internal["admissionPercent"] = 100
    changed_internal["level"] = "closed"
    cases.append(("internal_total_units", changed_internal, provider()))
    changed_internal = internal()
    changed_internal["remaining"]["admissionUnits"] = 12_000  # type: ignore[index]
    changed_internal["admissionPercent"] = 0
    cases.append(("internal_admission_ceiling", changed_internal, provider()))
    changed_provider = provider()
    changed_provider["day"] = "2026-08-13"
    cases.append(("day_mismatch", internal(), changed_provider))
    changed_provider = provider()
    changed_provider["usage"]["rowsWritten"] = True  # type: ignore[index]
    cases.append(("provider_usage_value", internal(), changed_provider))
    for code, left, right in cases:
        expect_error(code, left, right)

    # Exercise the CLI boundary and prove an injected extra-field value is not
    # reflected into its bounded stop diagnostic.
    with tempfile.TemporaryDirectory(prefix="mdkr-party-reconcile-") as temp:
        root = Path(temp)
        internal_path = root / "internal.json"
        provider_path = root / "provider.json"
        internal_path.write_text(json.dumps(internal()), encoding="utf-8")
        provider_path.write_text(json.dumps(provider()), encoding="utf-8")
        passed = subprocess.run(
            [sys.executable, str(TOOL), "--internal", str(internal_path),
             "--provider", str(provider_path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False,
        )
        assert passed.returncode == 0 and "PASS" in passed.stdout
        poisoned = copy.deepcopy(provider())
        poisoned["credential"] = "PRIVATE-CANARY-DO-NOT-ECHO"
        provider_path.write_text(json.dumps(poisoned), encoding="utf-8")
        stopped = subprocess.run(
            [sys.executable, str(TOOL), "--internal", str(internal_path),
             "--provider", str(provider_path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False,
        )
        assert stopped.returncode == 1
        assert stopped.stdout.strip() == \
            "reconcile_party_usage: STOP code=provider_schema"
        assert "PRIVATE-CANARY" not in stopped.stdout
        provider_path.write_bytes(b" " * (MODULE.MAX_INPUT_BYTES + 1))
        oversized = subprocess.run(
            [sys.executable, str(TOOL), "--internal", str(internal_path),
             "--provider", str(provider_path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False,
        )
        assert oversized.returncode == 1
        assert oversized.stdout.strip() == \
            "reconcile_party_usage: STOP code=input_too_large"

    print("test_party_usage_reconciliation: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
