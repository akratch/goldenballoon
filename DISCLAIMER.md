# Legal Disclaimer

**Golden Balloon is a decompilation and native source port project, created for
research, preservation, and educational purposes. It is an unofficial fan project
and is not affiliated with, endorsed by, or sponsored by any rights holder.**

## What this repository contains

- **Decompiled game code** — C that reproduces the behaviour of the original 1997
  Nintendo 64 kart racer. As with the
  [Super Mario 64](https://github.com/n64decomp/sm64) and
  [Ocarina of Time](https://github.com/zeldaret/oot) decompilation projects, this
  is a transformative, human-readable reimplementation of the game's logic. It is
  derived from the community
  [Diddy Kong Racing decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing)
  and includes the small, code-coupled data tables that are inseparable from that
  logic (object behaviour dispatch, collision and checkpoint tables, level index
  tables, display lists).
- **Original first-party work** — the native platform/port layer (rendering,
  audio, input, file I/O, frame pacing), the build system, the test and ROM-oracle
  tooling, and the documentation. This first-party work is offered under the
  [MIT License](LICENSE).

## What this repository does NOT contain

It contains **no game ROM** and **no bulk copyrighted assets** — no textures,
audio, music, sequences, models, fonts, sprites, level geometry, or logos. It
contains no proprietary SDK or library binaries. None of that data is present in
the repository or anywhere in its git history.

To build or run anything useful you must **provide your own copy of the original
game that you legally own and dumped yourself.** Every asset is read from that
copy at runtime, on your machine. Any file derived from it is git-ignored and must
**never** be committed or redistributed.

The browser build works the same way: you select your own ROM file in the page, it
is read **locally in your browser** and never uploaded, transmitted, or stored on
any server.

## Trademarks and rights

The original game — its title, characters, code, music, and all related assets —
is the property of its respective rights holders, which may include (without
limitation) **Nintendo** and **Rare** / **Microsoft**. All trademarks are the
property of their respective owners.

"Golden Balloon" is an unofficial project codename. It is not a product name of
any rights holder, and it is deliberately not the title of the original game.

## Acceptable use

- **Do** use this project with a ROM you legally own, for personal, educational,
  preservation, or research purposes.
- **Do not** use this project to obtain, share, or distribute the game or any of
  its assets.
- **Do not** redistribute builds that bundle copyrighted ROM-derived data. The
  release pipeline enforces this automatically — see `tools/check_no_rom.sh`,
  which fails closed if any shipped artifact contains N64 ROM header magic.
- **Do not** commercially use, sell, or promote the original game, its assets, its
  trademarks, or any build that bundles ROM-derived data.

## No warranty

This software is provided "as is", without warranty of any kind. See
[LICENSE](LICENSE). You are solely responsible for ensuring your use complies with
the laws of your jurisdiction.

If you are a rights holder and have a concern about this repository, please open an
issue or contact the maintainers, and it will be addressed promptly.
