# Platform acceptance routes

The parts of this port that cannot be closed by a check, because they need
hardware or a desktop session this project's machines do not have.

Each route below is a numbered sequence with an **exact expected observation**
per step and a result field. The point is that when the hardware does appear,
the pass is a checklist someone can execute in twenty minutes — not an
improvisation that produces a different answer each time and cannot be compared
against the last attempt.

## Rules for every route

1. **Every run passes `--headless-frames` only when the route says so.** These
   routes are about real windows and real displays, so most of them run the app
   normally. Where a route does invoke the binary from a shell, set
   `MDKR_AUDIO=0` unless the step is specifically about audible output.
2. **Capture the `[GPUINFO]` block on every route, pass or fail.** It names the
   backend chosen, every adapter considered, and the reason each was accepted or
   rejected. A failed route without it is a report nobody can act on — that is
   the whole reason the block exists.
3. **Record the result verbatim**, including "worked" — a route with no recorded
   result is an unexecuted route, and
   [`DEFINITION_OF_DONE.md`](DEFINITION_OF_DONE.md) must not claim otherwise.
4. **A route executed on one machine is evidence about that machine.** Do not
   generalise from an NVIDIA laptop to "Windows works".

## How to capture `[GPUINFO]`

```bash
MDKR_AUDIO=0 ./mdkr64 --headless-frames 1 --rom /path/to/your.z64 2>&1 \
  | grep '^\[GPUINFO\]'
```

It is emitted before the first rendered frame, so a build that fails to start
still produces it.

---

## Route L1 — Linux, Wayland session

**Why:** GL and WebGPU both build and the CI matrix includes ROM-free Linux
cells, but Wayland as opposed to X11 has never been exercised. Wayland differs
from X11 in window creation, fullscreen handover, DPI scaling and cursor
capture — four separate places SDL behaves differently.

**Preconditions:** a Wayland session (`echo $XDG_SESSION_TYPE` prints
`wayland`), a working GPU driver, a legally-dumped supported ROM.

| # | Step | Expected |
|---|---|---|
| 1 | Capture `[GPUINFO]`. | One block, a selected backend, at least one candidate, every candidate carrying a reason. |
| 2 | Launch the app normally. | Launcher window appears, correctly sized, not blank. |
| 3 | Point it at the ROM and press Play. | Race renders. |
| 4 | Press F11 (or the Fullscreen setting). | Fills the display, no letterbox artefacts, no crash on the transition. |
| 5 | Return to windowed. | Window returns to its previous size, not a default. |
| 6 | On a HiDPI output, check text and HUD. | Sharp, not upscaled from a low-resolution buffer. |
| 7 | Drag the window to a second output of a different refresh rate. | Frame limit "Match Display" follows the new rate. |
| 8 | Move the mouse over the game area during a race. | No cursor capture surprises; the cursor behaves as it does on X11. |
| 9 | Quit from the in-game overlay. | Clean exit, no `[CRASH]`/`[FATAL]`, exit code 0. |

**Result:** _not executed_

## Route L2 — Linux, X11 session

Identical to L1, on `XDG_SESSION_TYPE=x11`. Run it **in the same sitting as L1
on the same machine** — the comparison between the two is the evidence, and
running them weeks apart on different hardware produces two anecdotes instead.

**Result:** _not executed_

## Route L3 — Linux, ROM intake

**Why:** the launcher's file picker is dynamically loaded, and which path is
available depends on the desktop.

| # | Step | Expected |
|---|---|---|
| 1 | Capture the `[ROMINTAKE]` line at launch. | Names `portal`, `gtk`, or `paste-only`. |
| 2 | Click **Choose ROM File**. | On `portal` or `gtk`, a real file chooser opens. On `paste-only`, the UI says so rather than presenting paste as the only design. |
| 3 | Select the ROM through the chooser. | Accepted, validated, Play becomes available. |
| 4 | Restart and instead drag the ROM onto the launcher. | Also accepted. |

**Result:** _not executed_ — the picker itself is
[S4 task 4](sprints/S4-platform-breadth.md#task-4-linux-native-file-picker) and
is not built yet; today every Linux desktop reports `paste-only`.

## Route W1 — Windows, hybrid-graphics laptop

**Why:** a laptop with both an integrated and a discrete GPU is the
configuration most likely to launch on the wrong one, and the adapter policy
that fixes it has been tested only against synthetic candidate lists.

**Preconditions:** a laptop with Intel or AMD integrated plus a discrete GPU.

| # | Step | Expected |
|---|---|---|
| 1 | Capture `[GPUINFO]` with default settings. | **Both** adapters appear as candidates. The discrete one is selected, with a reason naming the class. |
| 2 | Set `Video.GraphicsAdapter=low-power`, relaunch, capture again. | The integrated adapter is selected, reason names low-power. |
| 3 | Set `Video.GraphicsAdapter` to a substring of the discrete adapter's name. | That adapter selected regardless of class. |
| 4 | Set it to a string matching nothing. | Falls back to auto, **starts anyway**, and the reason names the string that missed. |
| 5 | Set `Video.GraphicsBackend=d3d12`, then `vulkan`, then `gl`. | Each selects that backend or reports why it could not, and the app starts in all three cases. |
| 6 | Play a race on the discrete adapter. | Renders correctly, no device-loss messages. |
| 7 | Force a device loss if the driver allows (e.g. a driver restart). | Recovers or fails with a named reason; does not hang. |

**Result:** _not executed_

## Route W2 — Windows on Arm

**Why:** never run. The binary is x64; whether it runs under emulation at
acceptable speed, and what the WebGPU runtime does there, is entirely unknown.

| # | Step | Expected |
|---|---|---|
| 1 | Capture `[GPUINFO]`. | A block is produced at all — this is the first unknown. |
| 2 | Launch and reach the launcher. | Appears. |
| 3 | Play one race. | Completes; record the frame rate honestly, including if it is unplayable. |
| 4 | Record emulation overhead. | Note whether this is worth supporting or should be refused with a clear message. |

**Result:** _not executed_

## Route W3 — Windows, negative configurations

**Why:** a VM and an RDP session both present unusual or absent GPUs, and the
failure mode should be a clear message rather than a crash or a hang.

| # | Step | Expected |
|---|---|---|
| 1 | Launch inside a VM with no GPU passthrough. | Either runs on a software adapter, or refuses with a message naming what it looked for. Not a hang, not a silent exit. |
| 2 | Launch over RDP. | Same. |
| 3 | Capture `[GPUINFO]` in both. | Present, listing whatever was enumerated and why each was rejected. |

**Result:** _not executed_

## Route M1 — macOS, Apple silicon

**Why:** this is the developed-on path, so the route exists to make the
comparison with M2 meaningful rather than because it is doubtful.

| # | Step | Expected |
|---|---|---|
| 1 | `lipo -archs` on the bundle executable. | Lists `arm64` (and `x86_64` once the universal build lands). |
| 2 | Capture `[GPUINFO]`. | One block, backend named. |
| 3 | First-open: launch the downloaded DMG's app. | Behaves as the README's first-open note describes. |
| 4 | Play a race, save, quit, relaunch. | Progress preserved. |

**Result:** _not executed as a formal route_ — this configuration is covered
informally by daily development, which is not the same thing and should not be
recorded as if it were.

## Route M2 — macOS, Intel

**Why:** no Intel slice ships today
([S4 task 2](sprints/S4-platform-breadth.md#task-2-macos-universal-binary)), so
this route currently has nothing to run. It exists so that the moment the
universal build lands, the acceptance is defined.

| # | Step | Expected |
|---|---|---|
| 1 | `lipo -archs` on the executable **and every bundled dylib and framework**. | All list both `x86_64` and `arm64`. A universal executable with a single-slice dependency fails at launch with no useful message, so the dependencies are the point of this step. |
| 2 | `codesign --verify --deep --strict` the bundle. | Passes. |
| 3 | Launch on an Intel Mac. | Reaches the launcher. |
| 4 | Capture `[GPUINFO]`. | Backend named; note whether it differs from Apple silicon. |
| 5 | Play a race. | Renders; record the frame rate. |

**Result:** _not executed — no Intel artifact exists_

## Route A1 — Controller hotplug, any platform

**Why:** startup enumeration is exercised constantly; what happens **after**
startup is not. A pad that disconnects mid-race and leaves a stuck input is the
failure that actually loses races.

`check_input_hotplug.py` gates this automatically once
[S4 task 5](sprints/S4-platform-breadth.md#task-5-controller-hotplug-gate)
lands. Until then it is manual, and even afterwards the physical route is worth
running once because SDL's virtual joystick is not a real device.

| # | Step | Expected |
|---|---|---|
| 1 | Start a race with one pad. | Player 1 responds. |
| 2 | Connect a second pad mid-race. | Opened; reaches player 2's channel; player 1 unaffected. |
| 3 | Disconnect player 1's pad mid-race while holding a direction. | That channel reads **exact neutral**, immediately. Not the last-held value. |
| 4 | Confirm players 2–4 did not renumber. | Same channels as before. |
| 5 | Reconnect the pad. | Returns to its original channel. |
| 6 | Connect a fifth pad. | Ignored without error. |
| 7 | Trigger rumble on a disconnected channel. | No-op, no crash. |

**Result:** _not executed_

## Route D1 — Audible output matrix

**Why:** SDL cannot expose the hardware buffer and silence cannot prove speaker
output, so audible qualification is release-matrix evidence rather than
something a check can assert. This is already recorded as a residual in
[`DEFINITION_OF_DONE.md`](DEFINITION_OF_DONE.md).

| # | Step | Expected |
|---|---|---|
| 1 | Play a race through built-in speakers. | Music and effects audible, no crackle, no dropouts. |
| 2 | Repeat through USB audio. | Same. |
| 3 | Repeat through Bluetooth. | Same, allowing for the latency the transport imposes. |
| 4 | Repeat through HDMI. | Same. |
| 5 | Change output device while the game runs. | Recovers or fails with a named reason; does not wedge. |
| 6 | Repeat one arm on PAL. | The beat-aware cushion holds; no underruns. |

**Result:** _not executed_

---

## Recording a result

Replace the route's `**Result:**` line with the date, the machine, and what
happened — including the `[GPUINFO]` block. If a step failed, say which one and
what it did, and open an entry in [`open-items/`](open-items/README.md) with the
mechanism. A route recorded as "passed" with no detail is worth about as much as
an unexecuted one.

Then update the hardware list at the end of
[`DEFINITION_OF_DONE.md`](DEFINITION_OF_DONE.md), which points here. **Executed
means executed** — do not promote a code review, a reasonable expectation, or a
successful build to a machine check.
