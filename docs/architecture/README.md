# Architecture — subsystem specs

One file per subsystem of the port layer. These were written as forward-looking
milestone specs (M4–M8) and then updated in place as each wave landed, so each one
carries both the design intent and what was actually measured. The milestone
identity is kept in each file's title — it is how the commit log, `docs/STATUS.md` and
`docs/OPEN_ITEMS.md` refer to them.

| File | Was | Covers |
|---|---|---|
| [input.md](input.md) | `M4_INPUT_SPEC.md` | Keyboard + SDL controller path into the game's own controller struct, default bindings, save data, interactive frame pacing |
| [webgpu.md](webgpu.md) | `M4.5_WEBGPU_PLAN.md` | The qualified native WebGPU default and the only renderer in the browser. wgpu-native dependency, CMake wiring |
| [audio.md](audio.md) | `M5_AUDIO_SPEC.md` | The clean-room audio engine driving a software `aspMain` mixer: ADPCM decode, envelope mixing, resampling, reverb, host output |
| [race.md](race.md) | `M6_RACE_SPEC.md` | Getting into a controllable race: the deterministic menu route to gameplay, and the verified `MENU_ID` → trace-id mapping |
| [web.md](web.md) | `M8_WEB_SPEC.md` | The browser (wasm) build: why the architecture was already web-ready, the wasm32-specific defects found and fixed, ROM + saves in the browser |
| [online-multiplayer.md](online-multiplayer.md) | proposed online architecture | Launcher-owned deterministic rollback, private-room UX, local/online seat model, asset-blind room service and zero-cost reliability constraints |
| [camera-obstruction.md](camera-obstruction.md) | Native camera modernization plan | Desired-versus-resolved camera authority, projection-derived lens collision, terrain/object occluders, mode policy, rollout, and release gates |
| [taj-playable-mod.md](taj-playable-mod.md) | proposed mod architecture | A virtual playable Taj: easter-egg unlocks, carpet presentation, OP handling, persistence, compatibility boundaries, and execution gates |
| [playable-wizpig-terry-campaign.md](playable-wizpig-terry-campaign.md) | bonus-racer implementation campaign | Playable Wizpig and Terry: virtual identity, balance, vehicle policy, assets, persistence migration, regression controls, and release gates |
| [playable-wizpig-terry-integration.md](playable-wizpig-terry-integration.md) | bonus-racer integration guide | Exact implementation commit, product contract, subsystem ownership, conflict hotspots, completed qualification, known caveats, and post-merge checklist |

## Reading order

If you are new, read them in the order the port was built — each assumes the one
before it: `input.md` → `webgpu.md` → `audio.md` → `race.md` → `web.md`.

## Related documents outside this directory

These are architecture too, but they are referenced directly from source comments
or tooling, so they stay where they are:

- [`../multiplayer/README.md`](../multiplayer/README.md) — operational authority
  for the multiplayer achievement ladder, ownership, UX/security standards and
  S10–S13 execution order.
- [`../ref/online-lobby-protocol-v1.md`](../ref/online-lobby-protocol-v1.md) —
  executable launcher-owned room reducer, concurrency and ownership contract.

- [`../asset_swap_notes.md`](../asset_swap_notes.md) — per-asset-type endianness and
  LP64 layout coverage table. Cited by `platform/asset_swap.c` and `asset_swap.h`.
- [`../ref/dkr_asset_spec.md`](../ref/dkr_asset_spec.md) and
  [`../ref/asset_fileTypes/`](../ref/asset_fileTypes/) — on-disk asset layouts,
  the authority `asset_swap.c` is written against.
- [`../ORACLE.md`](../ORACLE.md) — the visual oracle harness, including the ways it
  has lied. Read it before trusting a fidelity score.
