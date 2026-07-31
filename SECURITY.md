# Security policy

Scoped honestly: this is a single-player game port, not a service.

## Threat surface

There is very little of one.

- **No network code.** The engine opens no sockets and makes no requests. The browser
  build fetches its own static files and nothing else.
- **No server.** The published page is static. Your ROM is read client-side, in your
  own browser, and is never uploaded, transmitted, or stored anywhere.
- **No privileged operation.** No installer, no driver, no elevated permissions. The
  only files written are the EEPROM save (`save/eeprom.bin`) and any dump you
  explicitly ask for with a flag or env var.
- **No credentials or personal data** are collected, stored, or transmitted.

What remains is memory safety. The engine is a native port of decompiled 1997 C
running on a modern 64-bit host, and it parses a ROM you supply. Malformed or
unexpected ROM data can crash it. Given the input is a file you already own and
chose to load, this is a robustness problem rather than a meaningful attack vector.

## Reporting

**Crashes, hangs, assertion failures and sanitizer reports: open a normal issue.**
They are bugs, and public discussion is how they get fixed here. Include what
[CONTRIBUTING.md](CONTRIBUTING.md#reporting-a-bug) asks for — the command line, the
ROM version, the renderer, and the full `[CRASH]`/`[FATAL]` output. A headless repro
with an input script is worth far more than a description.

If you believe you have found something genuinely sensitive — an issue that could
harm users if described publicly before it is fixed — do not open an issue. Contact
the maintainers privately via the repository's security advisory feature and allow
reasonable time for a fix.

Please **do not** attach a ROM, a ROM dump, or any ROM-derived data to a report.
Reports containing copyrighted game data will be deleted. Describe the ROM by
version (e.g. `us.v80`) instead.

## Not security issues

- The game not being byte-identical to the original.
- Rendering, audio or timing inaccuracy. Those belong in
  [docs/open-items/](docs/open-items/README.md).
- Needing your own ROM to play. That is deliberate and permanent — see
  [DISCLAIMER.md](DISCLAIMER.md).

## No warranty

This software is provided "as is", without warranty of any kind. See
[LICENSE](LICENSE).
