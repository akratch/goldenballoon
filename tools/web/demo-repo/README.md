# Golden Balloon — web demo

**▶ Play: https://akratch.github.io/golden-balloon/**

A native source port of the 1997 Nintendo 64 kart racer, compiled to WebGPU and
running entirely in your browser.

> ### Bring your own ROM
>
> **This repository contains no ROM and no game assets** — no textures, audio,
> music, models or level data, and none in its history. You supply a copy of the
> original game that you legally own and dumped yourself.
>
> Your ROM is read **locally in your browser**. It is never uploaded — the site is
> static and there is no server to receive it.

## What this repository is

A **publication target**, nothing more. It holds only built artifacts:

| | |
|---|---|
| `mdkr64_web.wasm` / `.js` | the compiled engine |
| `mdkr64_web.js.symbols` | the symbol map from that exact link, so a crash trace can be read |
| `mdkr-save-tools.wasm` / `.js` | the ROM- and renderer-free save codec/editor module |
| `index.html`, `style.css`, `mdkr64-shell.js`, `mdkr-save-ui.js` | the launcher shell and local save-management UI |
| `assets/` | project brand art |
| `build-info.json` | which source commit produced this build |

**There is no source code here** — no decompiled game code, no engine source.
This repository holds only the built web artifacts that GitHub Pages serves. The
source lives in its own repository; see the project README for the link.

## Do not send pull requests here

This repo is generated. Its contents are overwritten wholesale on every publish, so
any change made here is silently lost. Issues about *gameplay* are welcome; code
changes have to happen upstream.

## Requirements

A WebGPU browser — Chrome or Edge 113+. There is no WebGL fallback; the page tells
you up front rather than rendering black. Save backup, import, editing, and
recovery remain available when WebGPU is missing; they do not load the game
engine or require a ROM.

## Legal

An unofficial fan project for research, preservation and education, not affiliated
with or endorsed by any rights holder. The original game and all its assets are the
property of their respective owners, which may include Nintendo and Rare /
Microsoft. All trademarks belong to their owners. See [DISCLAIMER.md](DISCLAIMER.md).

The first-party engine and shell are MIT licensed — see [LICENSE](LICENSE). That
license covers our code only and grants no rights in the game.

If you are a rights holder with a concern, please open an issue; it will be
addressed promptly.
