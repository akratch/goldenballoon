# Save backup, import, editing, and recovery

Golden Balloon stores the original 4-Kbit EEPROM image and up to four virtual
Controller Paks locally. Browser IndexedDB persistence is convenient, but it is
not a backup: clearing site data, private browsing, storage eviction, or
changing browser/device can remove it. Download backups whenever the progress
or saved Time Trial ghosts matter to you.

All save processing is local. A save file is never uploaded, and the launcher
does not need a ROM, WebGPU, or the game engine to manage it.

In a native packaged app, live EEPROM, recovery points, and Controller Paks are
stored under `SDL_GetPrefPath("mdkr64", "mdkr64")/save`, never in the signed app
bundle or its working directory. `MDKR_SAVE_DIR` remains the explicit native
override for command-line tools and isolated tests. Non-packaged CLI builds
retain their CWD-relative `save/` default, and the browser remains `/save` on
IDBFS. A first packaged launch copy-migrates a complete known legacy save set
only when the destination directory is absent: it stages every file in a
sibling directory, atomically installs the whole directory, and never mutates
the legacy source. Interrupted stages created by this version are recognized by
destination-bound metadata and safely retried. Unrecognized legacy stages are
left intact and the app names the exact folder that needs manual recovery.

## Browser workflow

Open **Stored data** on the launcher:

- **Download backup** creates a versioned `.mdkr-save` file with the exact
  512-byte EEPROM payload and a SHA-256 integrity digest.
- **Download raw EEPROM** creates a byte-exact `.eep` file for emulator and
  native-port interoperability.
- **Download ghost Paks** creates a versioned `.mdkr-paks` bundle containing
  every stored controller-port image. The per-track ghost library (the
  `ghost-bank` directory beside the Pak files, one file per track and
  vehicle) is not part of the bundle yet; on desktop, back it up by copying
  the directory.
- **Import ghost Paks** validates every image through the game's C decoder and
  transactionally replaces the complete stored Pak set.
- **Import backup** accepts either format. Review the decoded slots and global
  data, then choose **Replace saved progress**.
- **Edit saved progress** works on a draft. Nothing is stored until **Apply
  changes** completes its persist, reload, and byte-verification transaction.
- **Recovery points** compares the current EEPROM against three automatic prior
  checksum-safe generations and the previous import/editor rollback. Restoring
  one uses the same reviewed, verified replacement transaction as import.
- **Erase saved progress** removes the EEPROM, rollback copies, automatic
  recovery points, Controller Paks, the per-track ghost library, and
  quarantined artifacts after an explicit confirmation.

The import target can also receive a dropped file or be opened from the keyboard.
The size limit is 64 KiB; raw EEPROM input must be exactly 512 bytes.
Controller Pak bundles have a separate 256 KiB limit. EEPROM and Pak backups
are intentionally separate, so download both when both kinds of data matter.

### Recovery and merge

A checksum-damaged save is never silently rewritten:

- raw export remains available so the original bytes can be retained;
- **Reset corrupt blocks** creates a preview in which only corrupt,
  independently checksummed blocks are reset;
- **Copy selected data into the current save** can copy individual Adventure
  slots, global options, or either record block from an import candidate.

Both operations create a new preview. They do not write until the normal
replacement transaction is confirmed.

### Transaction and rollback contract

Import and editor writes:

1. synchronize the browser store into the launcher filesystem;
2. stage and reread the candidate;
3. retain the current image as `eeprom.bin.previous`;
4. install and persist the candidate;
5. reload it from IndexedDB and verify exact bytes.

Any failure attempts to restore and persist the prior image. The rollback copy
lives in the same browser origin, so it does not survive complete site-data
loss; only a downloaded file does.

Pak bundle replacement extends the same contract across all four ports. Each
candidate is staged and reread, current port images are retained as
`.previous`, the complete set is persisted and reloaded, and every live image
or intended absence is verified. A failure at any stage restores the complete
prior set rather than leaving ports from two generations.

### Gameplay auto-save durability

The native-port EEPROM seam commits each independently checksummed save domain
as one complete replacement. In the browser, the game then waits for IDBFS to
acknowledge that generation before simulation continues. This prevents a page
refresh from capturing one of the checksum-invalid intermediate states that the
old five-write Adventure-slot path exposed.

Before replacing a live EEPROM generation, the engine retains up to three
distinct checksum-safe prior generations. If a later boot finds a damaged
block, it restores only that block from the newest usable recovery point and
keeps every newer valid block from the live image. A wrong-length live image can
be restored from the newest complete safe point. Rejected bytes are still
quarantined for diagnosis rather than discarded silently.

If WebGPU initialization or the running engine fails, the launcher first
finishes the engine's ordered persistence queue, stops its periodic writer, and
then gives storage ownership back to the independent save tools. Export,
recovery, and complete erase therefore remain available on the failure screen,
and a stale engine timer cannot recreate files after an erase.

## Native command-line interoperability

The `mdkr-save` target links the same codec and container implementation:

```bash
cmake -S . -B build
cmake --build build --target mdkr-save

build/mdkr-save inspect save/eeprom.bin
build/mdkr-save export save/eeprom.bin backup.mdkr-save
build/mdkr-save export-raw backup.mdkr-save restored.eep
build/mdkr-save import backup.mdkr-save save/eeprom.bin
build/mdkr-save recover damaged.eep recovered.eep
```

`import` uses a staged durable replacement, rereads the destination, and rolls
back on failure. Add `--recover-corrupt` only when explicitly choosing to reset
corrupt blocks during import. Keep the original damaged input as forensic data.

The portable container and raw EEPROM formats are defined by the shared codec
in `platform/save_codec.c` and covered by `tests/test_save_codec.c`.
The virtual Pak format, corruption policy, and original API bridge are defined
in [`VIRTUAL_CONTROLLER_PAK.md`](VIRTUAL_CONTROLLER_PAK.md).

## What is intentionally unavailable

- No save or ROM cloud service.
- No arbitrary hex editor or reserved-bit toggles.
- No semantic ghost editor or raw emulator `.mpk` converter. The launcher
  backs up and restores exact MDKR64 virtual Pak images.
- No "100%" preset until a reference-generated golden save defines every
  coupled progression flag.
- No editing while the game engine owns the mounted save.

## Contributor and release gates

Run the ROM-free browser custody gate after changing the codec, container,
launcher storage, or save UI:

```bash
tools/web/build_web.sh
python3 tests/check_browser_save_ui.py \
  --engine-dir build-web --shell-dir dist/web
```

It deliberately removes WebGPU, never selects a ROM, and verifies EEPROM exports,
hostile inputs, every injected transaction failure, corrupt recovery, automatic
recovery-point restore and erase, block merge, one-field edit containment,
accessibility/focus, wipe/import/reload, and zero upload. The full browser
runtime additionally delays an EEPROM persistence acknowledgment and requires
the frame counter to remain frozen, verifies every retained automatic
generation's checksum domains, exercises Pak import/export and every controlled
Pak rollback boundary, completely erases both stores, and reloads the exact
committed EEPROM bytes. The save UI gate is registered as `browser_save_ui` in
`tools/run_checks.py` and runs in the guarded Pages publication workflow.
