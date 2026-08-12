#!/usr/bin/env python3
"""Prove durable N64 storage is gated before every host-side mutation."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
STUBS = ROOT / "platform/stubs_dkr.c"
GATE = "mdkr_rollback_game_runtime_host_io_allowed(true)"


def function_body(source: str, signature: str, next_marker: str) -> str:
    start = source.index(signature)
    end = source.index(next_marker, start)
    return source[start:end]


def validate(source: str) -> None:
    eeprom_write = function_body(
        source, "s32 osEepromWrite(", "s32 osEepromLongRead("
    )
    eeprom_long_write = function_body(
        source, "s32 osEepromLongWrite(", "/* ======================================================================== *"
    )
    pak_store = function_body(
        source, "static int virtual_pak_store(", "static void virtual_pak_quarantine("
    )

    assert eeprom_write.index(GATE) < eeprom_write.index("memcpy(candidate"), (
        "EEPROM block writes must be refused before candidate mutation"
    )
    assert eeprom_long_write.index(GATE) < eeprom_long_write.index(
        "memcpy(candidate"
    ), "EEPROM long writes must be refused before candidate mutation"
    gate = pak_store.index(GATE)
    for operation in (
        "virtual_pak_paths(channel)",
        "mdkr_virtual_pak_encode(",
        "mdkr_fopen_utf8(",
        "fwrite(",
        "dkr_fs_replace(",
        "mdkr_persist_save_async(1)",
    ):
        assert gate < pak_store.index(operation), (
            f"Controller Pak rollback gate must precede {operation}"
        )


def main() -> None:
    source = STUBS.read_text(encoding="utf-8")
    validate(source)

    # Positive controls remove each independent storage-family gate.
    for signature in (
        "s32 osEepromWrite(",
        "s32 osEepromLongWrite(",
        "static int virtual_pak_store(",
    ):
        start = source.index(signature)
        gate = source.index(GATE, start)
        broken = source[:gate] + "rollback_gate_removed" + source[gate + len(GATE):]
        try:
            validate(broken)
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"positive control did not fail for {signature}")

    print("test_durable_rollback_firewall: PASS")


if __name__ == "__main__":
    main()
