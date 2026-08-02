# The native app shell

`platform/app/` is the in-process launcher, settings UI and in-game overlay for
the native builds. It is ported from the sister **mgb64** project's `src/app/`
(the established backflow practice — see [MGB64_BACKFLOW.md](MGB64_BACKFLOW.md))
and adapted to this game.

Native only. The browser build keeps its own JS shell (`dist/web/index.html`),
which already provides a launcher, ROM picker and settings surface there; the
wasm target is compiled with `MDKR_APP=OFF` and links no ImGui.

---

## How it decides what to do

The shell owns `main()`. `platform/main_pc.c`'s original `main()` is renamed to
`mdkr64_headless_main()` under `-DMDKR_APP` and is otherwise untouched.

The routing rule is **deny by default**: the launcher opens only for a bare
invocation (no arguments) or an explicit `--ui`. *Anything* else — one flag, a
positional ROM, a flag invented next year — delegates verbatim to
`mdkr64_headless_main()`.

This is deliberately inverted from mgb64, which keeps an allow-list of
automation flags and routes everything else (including `--rom`) into a launcher
it then seeds. That is the riskier direction here: this project's harness is
CLI-first, with roughly 197 `--rom` call sites across `tests/`, `tools/` and
`.github/`, many combining `--rom` with flags that are not obviously
"automation" (`--video-list`, `--video-set`, `--pure`, `--window-size`,
`--renderer`). One missed entry in an allow-list silently opens a window inside
a headless gate and hangs it. Deny-by-default makes "no existing invocation
changes behaviour" a property of the rule instead of a property of the list
being complete.

The cost is real and accepted: mgb64's direct-launch seeding (`--rom X` opening
a launcher pre-filled with `X`) is **not** ported. Here `--rom X` still means
"boot this ROM now", which is what every script and user already expects.

---

## Window and device handoff

The launcher creates the window and, on WebGPU, the device/surface. Pressing
Play registers them (`platform/host_window.c`) and then runs the ordinary engine
boot path by synthesising the argv a user would have typed
(`platform/app/engine_boot.cpp`). The engine adopts what it is given instead of
creating its own, so the game continues in the *same* window — the launcher does
not close and reopen.

At engine shutdown, an adopted window is released but never destroyed: the shell
is still running and owns it.

---

## Renderer handoff verdict

**WebGPU is the native default; both launcher handoffs and overlays are
exercised on macOS.** GL remains explicitly selectable for diagnostics while
its scene-parity work continues:

| Surface | WebGPU (native default) | GL diagnostic (`MDKR_RENDERER=gl`) |
| --- | --- | --- |
| Launcher UI | `gfx_webgpu_imgui.cpp` | `imgui_impl_opengl3` |
| In-game overlay | overlay pass inside `wgpu_end_frame` (`gfx_webgpu.c`) | `platformOverlayRender()` before `SDL_GL_SwapWindow` (`platform_sdl_min.c`) |
| Window adoption | device + surface + window | window + GL context |

The GL in-game path needed one new call site. mgb64 renders its GL overlay from
`gfx_end_frame` in the shared `gfx_pc.c` front-end; this project has its own
F3DDKR front-end (`gfx_pc_dkr.c`) and swaps buffers in `platform_sdl_present`
instead, so the overlay draw goes immediately before the swap there. It is a
no-op when no overlay hooks are registered, which is every CLI invocation.

The shell asks `mdkr_render_backend()` — the same resolver the engine uses — so
the launcher can never bring up a device the engine would refuse to adopt.

---

## Getting a ROM in

**Nothing on disk is ever searched.** An earlier version of this panel scanned
the working directory, `$HOME`, Downloads, Documents and Desktop and
auto-selected the first `.z64` it found. That was withdrawn after live macOS
validation, for three reasons:

1. It picked the **wrong ROM** when several dumps were present — "first match in
   scan order" is not "the one the player wants", and it happened before the
   player saw any UI.
2. Its in-app folder browser could not reach anything. It opened at the working
   directory (`"."`), and `parentOf(".")` returns empty, so no `..` row was ever
   emitted and there was no way to navigate out. Older `.app` builds also
   changed CWD to `Contents/Resources`, so the browser opened inside the signed
   bundle on an empty list with no exit.
3. Worst: `opendir()` on Downloads/Documents/Desktop trips macOS **TCC** and
   raises a system permission prompt before the player has asked for anything.
   For an unsigned fan-made source port that is alarming, and rightly so.

`rom_scan.{h,cpp}` is **deleted**, not disabled, and the panel calls no
directory-enumeration API at all. That is what makes "no permission prompt can
ever fire" a property of the code rather than a promise.

Four permission-free ways in, in the order a first-time player meets them:

| Path | Why it needs no permission |
| --- | --- |
| **Drag and drop** onto the window | `SDL_DROPFILE`. The drag is the user's own action; macOS grants access to what they dropped. |
| **Choose ROM File...** (native panel) | NSOpenPanel / GetOpenFileNameW. A file the user personally selects is TCC-exempt regardless of where it lives. |
| **Typed path** | The app opens exactly the one file named, and nothing else. |
| **Remembered ROM** | Stored in the app's own prefs file, which the app owns. |

Only a ROM that actually validates is remembered, so a refused revision can
never wedge the launcher into reproducing the same refusal every launch.

The identified revision (`US 1.1 (NTSC-U, Rev 1)`) is the largest text on the
ROM card, above the build tag, byte order and CRC pair — a wrong pick has to be
visible *before* Play, not discovered afterwards. A ready ROM gets an explicit
**Change ROM...** button; the acquisition controls stay hidden until then.

## Bundle resources and writable user data

The native entry point never changes CWD. When the executable is inside
`*.app/Contents/MacOS`, `platform/user_paths.c` registers the sibling
`Contents/Resources` directory as an immutable absolute root. The controller
mapping database is the only engine-opened payload in that resource tree;
icons, the privacy manifest, and signing metadata are consumed by macOS, not by
relative engine file I/O.

Mutable state is separate and always outside the signed bundle:

- `mdkr64.ini` is stored directly below
  `SDL_GetPrefPath("mdkr64", "mdkr64")`;
- EEPROM, automatic recovery points, and virtual Controller Paks are stored in
  its `save/` child;
- app-shell preferences and diagnostics already use that same SDL preference
  root.

`MDKR_VIDEO_CONFIG_PATH` (a complete filename) and `MDKR_SAVE_DIR` remain
explicit native overrides. They are used by automated/headless runs to isolate
state. A non-packaged command-line build retains the historical CWD-relative
defaults for the same reason. The browser remains `/save/mdkr64.ini` plus
`/save` on IDBFS.

On the first packaged launch only, when the new destination does not exist, the
path policy can copy legacy `mdkr64.ini` and known save files from the old
`Contents/Resources`/CWD locations. Save files are first copied into a sibling
staging directory and the complete directory is atomically renamed into place.
Migration never renames, deletes, or writes a legacy source; an existing new
destination always wins. If SDL cannot provide a writable preference root, the
package stops with a visible data-directory error instead of falling back into
the signed bundle.

---

## Settings are generated, not hand-written

`Settings_draw()` enumerates `mdkr_video_schema` and renders whatever it finds.
There is no second copy of the key list, so a new key cannot appear in the config
layer and be missing from the UI.

To make grouping a schema property rather than a UI opinion, `MdkrVideoSchema`
gained a `category` field (`Presentation` / `Fidelity` / `Pacing`). A new key
therefore cannot be added without deciding where a player would look for it.
The launcher presents Pacing first as **Frame rate & timing**, with the safe
**Original (Recommended / Proven)** choice visible on entry. Match Display,
numeric caps, and Uncapped are individually marked
**Experimental — Under Construction**. In 1.0.1+ those choices only affect host
pacing and input/event-pump opportunities, not unique visual FPS. They never
change gameplay timing or present duplicate images; the primary US 1.1 build
remains at its authored roughly 30 unique visual FPS. Their benefit may be
negligible, while higher settings can use more CPU. The panel directs players
to Original for the proven release behavior.

`Video.MotionSmoothing` remains a restart-scoped configuration seam, but the
production 1.0.1+ renderer resolves it to off. Delayed display-list replay is not
safe after the next task starts rewriting mutable viewport, matrix, vertex,
texture, and nested display-list storage, so the launcher must not imply that
this patch can produce interpolated visual frames.

Scope is presented honestly:

- **LIVE** keys apply immediately.
- **RESTART** keys (`Video.FrameLimit`, `Video.MotionSmoothing`,
  `Gameplay.SimulationCadence`, and others) are badged `restart`, saved, and
  shown as `Saved for next Play: Y (current: X)`. The Settings page keeps one
  primary action visible and renames it **Play with changes** while anything is
  staged. The UI never implies a restart-scope change took effect. Those scopes
  are not arbitrary — see the
  long comments in `platform/video_config.c` for why both present-pacing keys
  latch and why flipping them live is unsafe in *both* directions.
- Keys pinned by an environment variable or the command line are disabled and
  name the layer that owns them, because `mdkr_video_config_runtime_set()` would
  return `LOCKED` for them anyway.
- A Pure session is read-only (it never rewrites the ini), so the panel says so
  rather than offering controls that would silently do nothing.

Every committed edit routes through `mdkr_video_config_runtime_set()`, which
validates, persists and then publishes only the LIVE half. Text fields retain
their local value through validation/storage failure and commit on Enter or
blur. Enum and checkbox controls likewise retain the attempted value when a
write fails. The visible Retry control resubmits that same staged value after
write access is restored, without restarting the process. Sliders preview
locally while dragged and perform one durable transaction on release, so
dragging cannot fsync the configuration once per rendered frame.

ROM replacement follows the same last-known-good rule: a candidate is validated
and its preference is atomically saved before it replaces the active ROM. An
invalid or unsaved candidate is shown alongside the still-playable active ROM,
and Cancel always restores the prior view.

---

## Native accessibility contract

The supported native accessibility target for this release is keyboard and
gamepad operation, visible focus, scalable high-contrast UI, and restrained
motion. It does **not** claim a native VoiceOver, UI Automation, or other screen-
reader semantic tree: Dear ImGui does not expose one by itself. Full assistive-
technology semantics would require a native accessibility bridge and separate
platform qualification.

The supported behavior is explicit:

- keyboard navigation and activation are enabled throughout the launcher;
- the in-game F1 overlay uses Escape and controller B symmetrically: close an
  ImGui popup first, dismiss confirmation, leave Settings, then close overlay;
- UI scale is persisted from 0.75x through 2.00x and scales fonts, controls,
  spacing, and navigation from an unscaled baseline (never cumulatively);
- moving between displays rebuilds fonts before the next ImGui frame while the
  renderer's dynamic-texture lifecycle safely retires the old atlas;
- the minimum supported logical window is 640x480. Below the wide breakpoint,
  the navigation rail becomes top tabs, controls clamp to available width, and
  actions stack instead of clipping;
- Steel Blue focus/action state, Amber Gold navigation focus, light foreground
  text, and dark surfaces are checked by the capture-content gate; and
- the shell adds no decorative animation, auto-scrolling, or flashing content.

The release embeds one reviewed, redistributable Roboto Medium asset. Titles,
body text, and captions use distinct sizes and colors to establish hierarchy;
the app deliberately does not load an unreviewed system/remote Regular font,
which would make release rendering and licensing non-reproducible.

---

## Known limitations

**The overlay does not pause the simulation.** mgb64's does: its engine zeroes
`g_ClockTimer` when `platformOverlayWantsInput()` is set. That is a
GoldenEye-specific hook in *game* code. There is no equivalent seam here, and
adding one would mean editing `game/src`, which this work deliberately avoids.
So the overlay swallows input — the kart gets a neutral pad and coasts rather
than steering itself into a wall — and the footer says the race is still
running. Claiming "Paused" over a still-running race would be the dishonest
option. This is the one genuine parity gap.

**Return to Launcher performs an orderly process relaunch.** The overlay only
requests the transition. The engine then unwinds renderer/audio state, host
objects are released, the diagnostic tee restores stdout/stderr and joins its
reader, and only then does the platform replace the process. A failed relaunch
is shown visibly and returns non-zero instead of hanging on an orphaned pipe.

**No native file dialog on Linux.** macOS uses NSOpenPanel and Windows uses
GetOpenFileNameW (see "Getting a ROM in" above). There is no permission-free
native panel on Linux without a new hard build dependency — the
xdg-desktop-portal route needs libdbus at configure time — so `isAvailable()`
returns false there and the ROM panel draws no Browse button at all rather than
one that fails at runtime. Drag-and-drop and the typed path are the documented
paths on Linux, and both are always present on every platform.

---

## Gates

```
ctest -R app_
```

- `app_shell` — argv triage (including a census of every flag the real harness
  passes) and the ROM validator's failure paths.
- `app_schema` — every schema key has a category, label and help text, and
  round-trips by name. Fully headless.
- `app_shell_smoke` — drives the launcher for four frames and requires a
  captured frame on disk. `main_app.cpp` returns non-zero when a requested
  capture is missing, so this cannot pass without producing pixels.
- `app_settings_capture_content` and `app_settings_minimum_scale2_content` —
  decode the BMP, require dimensions, contrast, palette and content coverage,
  and prove blank/offscreen/invisible-foreground mutation controls fail.
- `app_ui_input_persistence` — uses SDL mouse + keyboard and an SDL virtual
  gamepad to select 240 through the real combo; reloads it in a second process;
  then proves a visible storage failure leaves Original unchanged and a retry
  through the rendered Retry button succeeds after write access is restored in
  that same process.
- `app_gl_swap_interval_fallback` — injects swap-interval rejection and requires
  the diagnostic GL launcher to continue under its bounded software pacer.

Set `MDKR_SKIP_GPU_TESTS=1` to skip GPU-dependent gates on a machine with no GPU.

`MDKR_APP_SMOKE_DROP=<path>` extends the same headless smoke loop: it
synthesizes one `SDL_DROPFILE` inside `AppHost::pumpAndShouldQuit()` partway
through the run, after SDL's platform-event translation boundary. The event is
then consumed by the same handler and ownership path as a live OS drop. This
avoids pushing a platform-reserved event through SDL2 compatibility layers,
which cannot portably translate an application-built SDL2 drop payload to
SDL3's different event layout.
`tests/check_shell_dropfile.py` (`python3 tools/run_checks.py --only
shell_dropfile`) drives it both directions — a supported ROM accepted and
persisted, a non-ROM file refused without a crash — against an isolated
`MDKR_APP_PREFS_DIR` so it never touches the real shared prefs file.

Per `CONTRIBUTING.md`, every launch — including windowed ones — must set
`MDKR_AUDIO=0`. Synthesis still runs; only the output device is suppressed.
