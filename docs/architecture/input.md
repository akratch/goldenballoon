# Input, menu interactivity and native window modes

> **This work has landed.** The document was written ahead of the M4 wave and
> is kept because it is still the clearest description of the input path and
> the binding conventions. Everything under "Goal" and "Acceptance" was
> achieved; read those sections as a description of what the build does, not
> as outstanding work.

## Goal
Boot to the main menu in a visible SDL window, navigate menus with keyboard and/or
SDL game controller, start a race, and reach in-race rendering. `--headless-frames`
keeps working for CI-style checks; interactive mode is the default when no
`--headless-frames` flag is given.

## Input path

- `game/src/joypad.c` keeps the original libultra SI flow: `osContInit`, then
  per-frame `osContStartReadData`/`osContGetReadData` filling four `OSContPad`
  records.
- `platform/platform_sdl_min.c` owns host input state and updates it in the SDL
  event pump before the retrace message is synthesized. The game therefore sees
  a stable pad snapshot after its retrace wake.
- SDL's GameController database first converts device-specific layouts into
  normalized A/B/X/Y, shoulders, clicks, D-pad, trigger axes, and stick axes.
  `platform/controller_mapping.c` then maps each normalized digital source
  through the durable `Input.Controller*` settings to an N64 button. Multiple
  sources may intentionally map to one action. `None` leaves a source unbound.
- Left-stick X/Y stays analog steering and is not converted to a digital
  binding. Right-stick directions are thresholded digital sources and default
  to the four C-buttons.

## Default bindings (mirror mgb64 conventions, see mgb64 src/platform input files)
Keyboard (P1):
- Arrows = analog stick (full deflection ±80; add gradual ramp only if menus feel
  too fast — DKR menus read stick as digital)
- X = A, Z = B, C = Z-trigger, Enter = Start
- A/D = L/R shoulder, Q/E/W/S + IJKL = C-buttons (IJKL primary)
- Esc = quit (host-level, not mapped to the game)
SDL_GameController (any connected, P1 first): left stick = stick, A=A, B/X=B,
Y=C-up, LT/RT=Z, shoulders=L/R, D-pad=N64 D-pad, right stick=C-buttons. Settings
can remap every digital source, including stick clicks, to any N64 digital
action or None. **Restore controller defaults** reinstates this table atomically.
- N64 button bit constants: use the game's own defs (game/include — grep
  A_BUTTON/B_BUTTON/Z_TRIG/START_BUTTON etc.), NOT SDL scancodes in game code.

## Rumble

The game-facing Rumble Pak capability continues to reflect the connected
device. `Input.RumbleEnabled=0` only mutes host output, so disabling vibration
does not change game behavior or make the Pak disappear. Light, Balanced, and
Strong resolve to 35%, 65%, and 100% SDL motor amplitude. Changing the setting
stops or refreshes a live request immediately; disconnect and shutdown still
cancel feedback.

## Window mode

`Window.Mode=fullscreen` uses SDL desktop fullscreen with the borderless flag
made explicit. It covers the current display without changing the monitor's
video mode and avoids Windows overlapped-window borders. The setting is applied
at window creation on startup, and Settings, F11, and Alt+Enter all use the same
durable transition. An SDL or persistence failure keeps/restores the prior mode.

## Save data
- EEPROM file backing already exists (save/eeprom.bin). Verify init_save_data's
  first-boot path writes defaults and menus read language/settings sanely.
- Controller Pak stays absent (ghosts disabled) — confirm menu tolerates.

## Frame pacing (interactive)
- DKR runs 30fps gameplay with 60Hz VI retrace. The shim's retrace synthesis is the
  pacer: target real-time 60Hz retrace messages (SDL_GL_SetSwapInterval(1) vsync
  where available; fall back to a clock-based limiter). Do not let headless mode
  sleep — it should run as fast as possible.

## Acceptance — all met

1. `./build/mdkr64` opens a window, boot flow reaches the main menu, and keyboard
   input navigates: highlight moves, A advances, B backs out.
2. Start Adventure/Tracks flow far enough to load into a race level; report how far
   in-race gets (rendering + racer control), crashes documented precisely.
3. `--headless-frames 600 --dump-frames` still exits 0; spot-check dumps show the
   menu progressing (input can be scripted via a simple `--input-script file` of
   frame:button pairs if useful for automation — optional, only if cheap).
