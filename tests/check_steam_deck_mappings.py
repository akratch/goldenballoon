#!/usr/bin/env python3
"""Check the shipped Linux Deck mappings through SDL, not just as text.

The launcher loads this database before ROM selection. Its generic virtual-pad
UI test cannot prove a raw Steam Deck button becomes SDL's semantic A/B/D-pad.
Run on Linux with the same SDL2 used by the build or packaged AppRun.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
from pathlib import Path
import sys


EXPECTED = {
    "03000000de2800000512000010010000": {
        "a": "b3", "b": "b4", "dpup": "b16", "dpdown": "b17",
        "dpleft": "b18", "dpright": "b19",
    },
    "03000000de2800000512000011010000": {
        "a": "b3", "b": "b4", "dpup": "b16", "dpdown": "b17",
        "dpleft": "b18", "dpright": "b19",
    },
    "03000000de280000ff11000001000000": {
        "a": "b0", "b": "b1", "dpup": "h0.1", "dpdown": "h0.4",
        "dpleft": "h0.8", "dpright": "h0.2",
    },
}


class Guid(ctypes.Structure):
    _fields_ = [("data", ctypes.c_uint8 * 16)]


def fields(line: str) -> dict[str, str]:
    parts = line.strip().split(",")
    return dict(part.split(":", 1) for part in parts[2:] if ":" in part)


def check() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--sdl", help="SDL2 shared library; defaults to system SDL2")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this gate verifies Linux mappings and must run on Linux")
    database = args.database.resolve(strict=True)
    lines = database.read_text(encoding="utf-8").splitlines()
    records: dict[str, dict[str, str]] = {}
    for line in lines:
        guid = line.split(",", 1)[0].lower()
        if guid in EXPECTED:
            if guid in records:
                raise AssertionError(f"duplicate Linux controller GUID {guid}")
            records[guid] = fields(line)
    for guid, controls in EXPECTED.items():
        record = records.get(guid)
        if record is None or record.get("platform") != "Linux":
            raise AssertionError(f"missing Linux mapping for {guid}")
        for button, binding in controls.items():
            if record.get(button) != binding:
                raise AssertionError(
                    f"database {guid} {button}: {record.get(button)!r} != {binding!r}")

    library = args.sdl or ctypes.util.find_library("SDL2")
    if not library:
        parser.error("SDL2 shared library not found; pass --sdl")
    sdl = ctypes.CDLL(str(library))
    sdl.SDL_Init.argtypes = [ctypes.c_uint32]
    sdl.SDL_Init.restype = ctypes.c_int
    sdl.SDL_Quit.argtypes = []
    sdl.SDL_RWFromFile.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    sdl.SDL_RWFromFile.restype = ctypes.c_void_p
    sdl.SDL_GameControllerAddMappingsFromRW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    sdl.SDL_GameControllerAddMappingsFromRW.restype = ctypes.c_int
    sdl.SDL_JoystickGetGUIDFromString.argtypes = [ctypes.c_char_p]
    sdl.SDL_JoystickGetGUIDFromString.restype = Guid
    sdl.SDL_GameControllerMappingForGUID.argtypes = [Guid]
    sdl.SDL_GameControllerMappingForGUID.restype = ctypes.c_void_p
    sdl.SDL_free.argtypes = [ctypes.c_void_p]
    sdl.SDL_GetError.restype = ctypes.c_char_p
    if sdl.SDL_Init(0x00002000) != 0:  # SDL_INIT_GAMECONTROLLER
        raise RuntimeError(f"SDL_Init: {sdl.SDL_GetError().decode()}")
    try:
        stream = sdl.SDL_RWFromFile(str(database).encode("utf-8"), b"rb")
        if not stream:
            raise RuntimeError(f"SDL database open: {sdl.SDL_GetError().decode()}")
        loaded = sdl.SDL_GameControllerAddMappingsFromRW(stream, 1)
        if loaded < 0:
            raise RuntimeError(f"SDL mapping load: {sdl.SDL_GetError().decode()}")
        for guid, controls in EXPECTED.items():
            parsed = sdl.SDL_JoystickGetGUIDFromString(guid.encode("ascii"))
            pointer = sdl.SDL_GameControllerMappingForGUID(parsed)
            if not pointer:
                raise AssertionError(f"SDL has no mapping for {guid}")
            try:
                mapping = ctypes.string_at(pointer).decode("utf-8")
            finally:
                sdl.SDL_free(pointer)
            actual = fields(mapping)
            for button, binding in controls.items():
                if actual.get(button) != binding:
                    raise AssertionError(
                        f"SDL {guid} {button}: {actual.get(button)!r} != {binding!r}")
    finally:
        sdl.SDL_Quit()
    print(f"PASS: SDL resolves A/B/D-pad for two Deck GUIDs and Steam Virtual "
          f"from {database.name} ({loaded} new mappings)")


if __name__ == "__main__":
    check()
