# Virtual Controller Pak and ghost custody

**Implemented:** `774d2e4`  
**Scope:** original DKR ghost save/load, four local controller ports, native and
browser persistence, browser backup/import, and host haptics.

## Player contract

Each present controller owns an independent virtual Controller Pak:

| Port | Stored image |
|---:|---|
| 1 | `save/controller-pak-1.mdp` |
| 2 | `save/controller-pak-2.mdp` |
| 3 | `save/controller-pak-3.mdp` |
| 4 | `save/controller-pak-4.mdp` |

The image is created only when the game first mutates the Pak. Saving a Time
Trial ghost through the original post-race **Save Ghost** option writes the
image; a later process loads the ghost through the original game path.

In the browser, **Download ghost Paks** creates one versioned
`.mdkr-paks` JSON bundle containing every stored port image. **Import ghost
Paks** treats the bundle as a complete replacement set: a stored port absent
from the bundle is removed after confirmation. Current images are retained as
local `.previous` rollback files. Adventure progress remains in the separate
EEPROM backup, so players who care about both should download both files.

All processing is local. Neither Pak data nor the ROM is uploaded.

## On-disk contract

`MDKRPFS1` is an explicit byte format, not a serialized host struct:

- exact image size: 32,192 bytes;
- usable N64 Pak payload: 31,488 bytes, in 256-byte blocks;
- directory capacity: 16 files;
- 64-byte header and sixteen 40-byte directory entries;
- big-endian integer fields;
- monotonically changing 64-bit generation;
- SHA-256 over the complete image with the digest field zeroed;
- zeroed reserved fields and unused payload.

The decoder authenticates the image, then independently checks file count,
rounded sizes, extents, overlap, gaps, duplicate identities, hidden tails, and
reserved bytes. Decode is atomic: a rejected candidate cannot partially modify
the live Pak.

This is an MDKR64 custody format. Raw emulator `.mpk` images have a different
directory/inode/checksum layout and are not accepted or mislabeled as
compatible.

## Mutation and failure contract

Allocation, deletion, and writes use a copy of the live Pak. The candidate is
encoded to a temporary file, flushed, synchronized where the host supports it,
renamed into place, and only then committed to memory. In the browser, the game
awaits the corresponding IDBFS persistence request before continuing.

A malformed stored image is never interpreted as ghost data. It is moved to a
numbered `.bad.N` quarantine file and the current session reports bad data until
the Pak is explicitly reformatted. Browser bundle import validates every image
through the same C decoder used by the game, stages all four ports, persists and
reloads the set, and verifies exact bytes. Any failure restores the prior set.

The launcher's complete erase operates on every file in `/save`, including
EEPROM generations, Pak images, rollback files, and quarantines, and verifies
that no artifact remains.

## Original API bridge

The `osPfs*` boundary implements initialization, presence, allocation, find,
delete, read/write, file state, free space, directory counts, checking, and
reformatting with original-style error codes and quota behavior. The dormant
DKR ghost parser was hardened before enabling it:

- all seven directory offsets are validated before pointer formation or
  subtraction;
- record node counts, spans, vehicle/character IDs, and checksums are bounded;
- constructed files are zero-initialized and validated before persistence;
- enumeration has an explicit six-element output capacity;
- deletion correctly accepts directory slot zero;
- fixed-width Pak names no longer write a terminator past their 16-/4-byte
  arrays.

Virtual storage and rumble coexist at the host boundary. SDL haptics are used
when available; browser controllers use their Gamepad vibration actuator.
Disconnect and shutdown cancel active feedback. A device without haptics keeps
fully functional controls and storage.

Native Settings exposes rumble as a player comfort preference. Off sends zero
amplitude and immediately cancels a live effect while the game still sees the
connected Rumble Pak. Light, Balanced, and Strong use 35%, 65%, and 100% motor
amplitude respectively; DKR remains responsible for the authored effect timing.
The selection persists in `mdkr64.ini` and is independent of presentation mode.

## Evidence and remaining matrix

The implementation passed:

- 22/22 native CTests, including format round trips, full capacity/directory,
  delete/compaction, digest corruption, authenticated invalid extents, and
  atomic decode;
- native and linked wasm builds plus a clean 300-frame ASan fresh-save boot;
- a full three-lap Time Trial and EEPROM reload;
- the real post-race **Save Ghost** path, producing a 32,192-byte image, followed
  by a fresh-process race that loaded and advanced the stored ghost;
- real Chromium import/export, all five controlled Pak transaction failures
  with exact rollback, complete erase, and the full 3,600-frame WebGPU runtime.

Still external to the available test host: physical ports 2–4, disconnect and
reconnect during Pak I/O, the range of browser haptic implementations, and
Windows/Linux haptic runtime behavior. Those are release-matrix evidence gaps,
not known missing code paths.
