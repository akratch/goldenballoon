# A save written by the published 1.7.0 build

These files pin what a player's 1.7.0 save means to every later build. They
were written by the released game, not constructed: the v1.7.0 binary played
a new game, and what it persisted is what is here.

| File | What it is |
| --- | --- |
| `eeprom.hex` | The 512-byte `eeprom.bin` 1.7.0 wrote, as annotated hex |
| `eeprom.mdkr-save` | 1.7.0's own `mdkr-save export` of that image, verbatim |
| `inspect-1.7.0.json` | 1.7.0's own `mdkr-save inspect` of that image, verbatim |

Consumers: `tests/test_save_v170_fixture.c` (ROM-free: `save_codec.c` and
`save_container.c`), `tests/test_save_v170_inspect.py` (ROM-free: this tree's
`mdkr-save`), and `tests/check_save_v170_compat.py` (the game's own codec,
which needs the ROM's level table).

## How the image was made

The build: tag `v1.7.0` (`106bad37`), checked out clean into its own worktree
and configured with exactly the `MDKR_*` options and SDL of the 1.8 candidate's
`build-rel` (Release). The ROM: US v80, supplied locally and not part of this
repository.

The route is `tests/check_adventure_race_loop.py`'s, unchanged at `v1.7.0`: a
new game from an empty save directory, the intro, Timber's Island, the golden
balloon by the Dino Domain door, **the Dino Domain door**, the lobby, **one full
three-lap race of Ancient Lake**, back to the lobby and out to the island. The
game saved on its own along the way; nothing forced the result. Run from an
empty working directory:

```sh
MDKR_AUDIO=0 MDKR_SIMULATION_CADENCE=enhanced MDKR_SYNTH_FIELDS=1 \
MDKR_TRACE=1 MDKR_AUTOPILOT=1 MDKR_TEST_POSTRACE_OPTION=1 \
MDKR_DRIVE_ROUTE="0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12:-3381,1946:-2519,1516:-1858,1099;12:E5:E0" \
MDKR_SAVE_DIR="$PWD/save" MDKR_VIDEO_CONFIG_PATH=/dev/null \
  <v1.7.0 build>/mdkr64 --headless-frames 17000 \
  --input-script tests/input_scripts/adventure_race_loop.txt \
  --rom baserom.us.v80.z64
```

What 1.7.0's trace says happened, in order: the intro (level 36), the island
(level 0) at frame ~5867, balloon 10 collected (total 1), the exit to the Dino
Domain lobby (level 12) taken, the exit to Ancient Lake (level 5) taken, a
natural first place on lap 3 (`natural=1`), the lobby again through the Ancient
Lake door (entrance 3), and the island through the Dino Domain exit
(entrance 2). Two runs wrote byte-identical images.

`eeprom.mdkr-save` and `inspect-1.7.0.json` come from `mdkr-save` compiled
from the same `v1.7.0` sources (`tools/mdkr_save_cli.c` with the codec,
container, tools-core, virtual-pak and SHA-256 sources, version string
`1.7.0`): `mdkr-save export eeprom.bin eeprom.mdkr-save` and
`mdkr-save inspect eeprom.bin`.

## What the bytes say

1.7.0's report, which the tests hold every later build to:

- slot 1 valid, file name `AAA`; slots 2 and 3 erased (`0xFF`);
- save index 3 -- Ancient Lake in the ROM's save ordering -- is 2 (cleared),
  every other course 0;
- balloons `2, 1, 0, 0, 0, 0` (the island balloon and the race balloon both
  count toward world 0);
- world flags `1027, 54, 0, 0, 0, 0`, the per-world flags the route set (bit
  10 of the island's 1027 is balloon 10);
- cutscene flags `262144`; Taj, trophies, bosses, both amulets and keys all 0;
- the settings word and both record blocks never written.

## Why this may be committed

`.gitignore` keeps saves out of the tree and the release guard rejects new
binary files, so the image is text. It carries no ROM-derived bytes: a slot is
an MSB-first bit stream of progress fields (course states, counters, flags, a
file name) and a checksum; the other blocks are erase patterns.
`tools/check_no_rom.sh` passes on `eeprom.bin` and `eeprom.mdkr-save`.

That argument does not hold for a Time Trial, which is why none is here. The
first record write fills every untouched (course, vehicle) entry of both record
blocks with the ROM's default times and initials (`ASSET_MISC_23`), and each
block ends in two bytes the codec never writes, which `write_eeprom_data()`
takes from an uninitialised allocation and sums into the checksum. Records and
the Controller Pak ghost are therefore produced by the 1.7.0 build at test time,
by `tests/check_previous_release_parity.py`, and never stored.

## What this fixture cannot see

One door and one race reach neither trophies nor amulets nor keys, so those
fields are zero here. A change that swaps two all-zero fields of the same total
width moves no bit this image sets; the synthetic cases in
`tests/test_save_codec.c` and the game's own `MDKR_CUSTOM_SAVE_SELFTEST` cover
those fields with non-zero values.

## `eeprom.hex` format

A `size N` line, then `OFFSET: BYTES` rows in hex; `#` starts a comment. Every
byte appears exactly once -- the readers (`harness_utils.read_hex_fixture` and
the C reader in `test_save_v170_fixture.c`) refuse gaps and overlaps, so a
truncated or hand-edited file fails loudly instead of reading as zeros.
