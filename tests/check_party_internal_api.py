#!/usr/bin/env python3
"""Keep every Worker-to-Durable-Object call on the skew-safe v1 envelope."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SERVICE = ROOT / "services/party/src"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    worker = (SERVICE / "worker.ts").read_text(encoding="utf-8")
    calls: list[int] = []
    cursor = 0
    while True:
        cursor = worker.find(".fetch(", cursor)
        if cursor < 0:
            break
        calls.append(cursor)
        require("internalRequest(" in worker[cursor:cursor + 256],
                f"Worker object fetch at byte {cursor} lacks the v1 envelope")
        cursor += len(".fetch(")
    require(len(calls) == 15,
            f"review the frozen Worker/object call census: expected 15, found {len(calls)}")
    require(worker.count("internalRequest(") == len(calls),
            "an internal envelope is detached from the Worker/object call census")

    guarded = ["party-budget.ts", "party-room.ts", "party-code-directory.ts",
               "match/match-room.ts"]
    for relative in guarded:
        source = (SERVICE / relative).read_text(encoding="utf-8")
        fetch = source.index("override async fetch(request: Request)")
        guard = source.index("rejectUnsupportedInternalApi(request)", fetch)
        later = [position for token in ("this.ctx.storage", "readJson<", "new URL(")
                 if (position := source.find(token, fetch)) >= 0]
        require(later and guard < min(later),
                f"{relative} checks the internal version after parsing/storage access")

    print("check_party_internal_api: PASS — 15/15 Worker/object calls use v1; "
          "all four objects reject unknown versions before parse/storage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
