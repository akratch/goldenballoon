#!/usr/bin/env python3
"""Keep every Worker-to-Durable-Object call on the skew-safe v1 envelope."""

from pathlib import Path
import re


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
    require(len(calls) == 16,
            f"review the frozen Worker/object call census: expected 16, found {len(calls)}")
    require(worker.count("internalRequest(") == len(calls),
            "an internal envelope is detached from the Worker/object call census")

    guarded = ["party-budget.ts", "party-room.ts", "party-code-directory.ts",
               "match/match-room.ts"]
    for relative in guarded:
        source = (SERVICE / relative).read_text(encoding="utf-8")
        declaration = re.search(r"override (?:async )?fetch\(request: Request\)", source)
        require(declaration is not None,
                f"{relative} has no recognizable Durable Object fetch entry")
        fetch = declaration.start()
        guard = source.index("rejectUnsupportedInternalApi(request)", fetch)
        later = [position for token in ("this.ctx.storage", "readJson<", "new URL(")
                 if (position := source.find(token, fetch)) >= 0]
        require(later and guard < min(later),
                f"{relative} checks the internal version after parsing/storage access")

    room_source = (SERVICE / "party-room.ts").read_text(encoding="utf-8")
    require("const targetAttachment = socketAttachment(target)" in
            room_source and
            "targetAttachment.controllerId !== value.to" in room_source,
            "Phone Party host signaling is no longer attachment-targeted")
    require("export function deliverPartySocketText(" in room_source and
            'deliverPartySocketText(socket, message, "state_delivery_failed")' in
            room_source and
            "catch { /* A broken peer cannot abort state cleanup" in room_source,
            "Phone Party socket failure can escape an authoritative commit")
    native_queue = room_source.split("private enqueueNativeHostCommand(", 1)[1]
    native_queue = native_queue.split("private async reserveNativeCommand", 1)[0]
    require("this.ctx.blockConcurrencyWhile(async () =>" in native_queue and
            native_queue.index("blockConcurrencyWhile") <
            native_queue.index("applyNativeHostCommand"),
            "Phone Party native/HTTP mutations can interleave across awaits")
    worker_source = (SERVICE / "worker.ts").read_text(encoding="utf-8")
    require(worker_source.count("partyInviteRemainingMs(") == 2 and
            "inviteGeneration: state.inviteGeneration" in worker_source,
            "Phone Party responses no longer carry actual TTL/generation authority")
    security_source = (SERVICE / "security.ts").read_text(encoding="utf-8")
    require("export async function readRequestBytes(" in security_source and
            "request.body.getReader()" in security_source and
            "total + value.byteLength > maxBytes" in security_source and
            "await reader.cancel()" in security_source and
            'new TextDecoder("utf-8", {fatal: true, ignoreBOM: true})' in
            security_source and
            "request.arrayBuffer()" not in worker_source,
            "service request bodies are no longer streaming/fatal-UTF8 bounded")

    print("check_party_internal_api: PASS — 16/16 Worker/object calls use v1; "
          "four object guards, exact invite authority and targeted phone signaling")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
