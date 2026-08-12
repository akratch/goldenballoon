# Multiplayer UX patterns and copy catalog

Status: **normative v1 UI contract**. Browser and native surfaces use these
labels, state meanings and recovery priorities. Wire ids remain in the protocol;
raw ids, provider terms, ICE/STUN/TURN language and quota numbers never appear
in primary player copy.

## Experience principles

1. **Local is immediate.** Play here is visible and enabled without a network
   probe. Every network failure keeps it as the first stable recovery.
2. **One decision per scene.** A clear primary action advances the journey; Back
   or Cancel is always available during a wait.
3. **Seats are physical custody, not race identity.** Controller number/color
   answers “which controls are mine?” Local play keeps the original in-game
   character flow; online character/vehicle choices live in the launcher and
   freeze into the match descriptor before the engine starts.
4. **Safety is visible.** A disconnected controller visibly releases all input.
   A spinner never leaves controls looking active.
5. **Quality is described calmly.** Use Direct, Connected, Reconnecting and
   Limited connection. Put RTT/loss/transport details behind a disclosure.
6. **The launcher owns the journey.** The game never grows QR, account, room,
   matchmaking or provider UI. The session bridge receives only frozen match
   state and canonical pad samples.

## Home and route hierarchy

| Route | Primary action | Secondary action | Network rule |
|---|---|---|---|
| Home, ROM absent | Choose ROM | Learn what is needed | No multiplayer actions pretend readiness. |
| Home, ROM ready | **Play here** | **Online room** | Play here never waits for service health. |
| Local setup | **Start game** | Add phone controllers; Back | Internet is disclosed only beside Add phones. |
| Online entry | **Create private room** | Join room; Back | No public/quick-match affordance in the $0 profile. |
| Online lobby | **Ready** / host **Start race** | Invite, connection details, Leave | Start is gated by explicit preflight. |
| Results | **Race again** | Change track; Return home | Room/session remains alive until explicit leave. |

Role-specific links are never ambiguous: `/controller/#…` makes this device a
controller; `/room/#…` joins a game-running endpoint. Wrong-role detection names
the role and offers one action: **Open controller** or **Open game room**.

## Controller tile

Every local seat uses the same component on browser and native launchers:

```text
┌─────────────────────────────────┐
│ 1  Sky blue       Phone         │
│ Alex's phone      Connected  ✓  │
│ [Test controls]   [Remove]      │
└─────────────────────────────────┘
```

Required fields are number, non-color color name/marker, source kind, optional
device name, state text and contextual action. Source labels are exactly
**Keyboard**, **Gamepad**, **This screen**, **Phone**, and **Available**.
Controller colors are also named and numbered; color is never the only mapping.

| State | Status | Action |
|---|---|---|
| Empty | Available | Add/connect source |
| Pending phone | Waiting for approval | Approve / Decline in pending list |
| Approved, channel opening | Connecting phone… | Remove |
| Direct channel ready | Connected | Test controls / Remove |
| Input test passed | Ready | Test again / Remove |
| Transport lost | Reconnecting — controls released | Remove |
| Removed/expired | Available | Add/connect source |

Focus does not jump when a tile changes state. Removal returns focus to that
tile's Add action. Newly pending phones are announced politely; errors and
controller release use assertive announcements.

## Add phone controllers dialog

Title: **Add phone controllers**  
Instruction: **Scan to use this phone as a controller**  
Privacy: **Internet is needed to pair. Controller input goes directly to this
display.**  
Fallback label: **Or enter this six-digit code**  
Timer: **Invite expires in 1:42**  
Actions: **Extend 2 minutes**, **Start local game**, **End controller room**.

The QR is black on white, ECC Q, with a four-module quiet zone and no logo.
Code text is selectable and grouped `123 456`, but its accessible name reads all
six digits individually. The dialog opens focused on its heading/Close action
and traps keyboard focus. **Close** and Escape revoke the displayed invite but
preserve approved phone leases; only **End controller room** disconnects them.
Ending requires a separate confirmation whose initial focus is **Keep
controller room** and which names the disconnected phones plus surviving local
inputs. A later open rotates to a fresh QR/code. Focus returns to the invoking launcher
or in-game control. Starting the game moves focus to the game canvas instead of
a hidden launcher action.

One displayed QR intentionally admits multiple friends during its two-minute
window. Each phone appears independently and cannot send input before approval.
**Extend** rotates both QR and code and says: **Invite extended. The previous QR
code is no longer valid.**

Pending row:

```text
Alex's phone
Pairing phrase: Bright-Balloon Calm-Forest
[Controller 2 — available ▾]
[Approve] [Decline]
```

Approval stays disabled while key agreement says **Verifying…**. The first
unoccupied slot is recommended; choosing an occupied local slot explicitly says
**replace Keyboard**, **replace This screen**, or **replace Gamepad**. Both
people must compare the phrase; copy never tells them to approve a mismatch.

## Phone controller states

| State | Heading / essential copy | Primary | Escape/recovery |
|---|---|---|---|
| Code entry | Enter the code from the display | Join controller room | Explain six digits inline; no destructive reset. |
| Opening | Joining controller room… | — | Cancel |
| Waiting | Check the display / Pairing phrase: … | — | Leave |
| Assigned | Controller N / Press Go to test | Press Go | Leave |
| Ready | Connection works | Use controller | Test again / Leave |
| Active | Controller N | Touch controls | Settings / Leave |
| Reconnecting | Reconnecting / Controls are safely released | — | Leave |
| Duplicate | This controller is already open in another tab | Use this tab | Leave |
| Terminal | Exact reason | Enter another code | Return to instructions |

Leave from Waiting, Assigned, Active or Reconnecting always opens a confirmation
focused on **Keep controller**. Nothing neutralizes or disconnects until **Leave
controller room** is chosen. The resulting terminal state says **Controller
disconnected** rather than mislabeling a voluntary leave as an expired invite.

The controller is a control surface, not a miniature game UI. The analog stick
tracks the captured pointer 1:1; button feedback begins on pointer-down. Each
pointer owns one control until release/cancel. `pointercancel`, visibility loss,
page hide, channel loss, overflow and leave all publish exact neutral. A
three-finger hold exposes Leave when full-screen browser chrome is difficult.

Settings are local and immediately previewed: Left/right-handed, Control size,
Show labels, Haptics and Keep screen awake status. Haptics/Wake Lock/orientation
are enhancements and may fail without blocking play. Accelerate+Steer+Drift,
Accelerate+Steer+Item and Brake+Steer must be comfortable in real-hand review.

## Online room patterns

The UI can be implemented against fake adapters before A3, but the production
Start action remains absent/disabled until rollback `GO`.

### One executable state-and-copy contract

`platform/online/lobby_view_model.*` is the shared pure projection for native
and browser room views. It accepts only validated `MdkrSessionState`, optional
validated `MdkrOnlineLobby`, a local endpoint id, a stable product failure and
local release policy. It emits title, explanation, calm status, primary/
secondary/Cancel controls, timeout recovery, announcement priority and bounded
counts. UI adapters render this model and dispatch its typed actions; they do
not invent copy or infer room transitions from provider callbacks.

The native launcher binding lives in `platform/app/ui_online_room.*`. With no
test flag it renders an honest unavailable state and performs no network work;
`MDKR_APP_ONLINE_FAKE=1` is an offline design/evidence adapter only. Its 42-case
gallery covers every view and typed failure, its speech walk enumerates every
title/action set, and all 25 public actions activate through keyboard and
gamepad. The browser launcher has the same honest zero-I/O production entry and
hands local recovery to the existing ROM and Phone Party owners. Its explicit
evidence adapter renders a standalone 34 KiB Wasm projection compiled from the
same C reducer/view model; JavaScript owns DOM semantics and launcher routes,
never room transitions. The full 42-case browser/native correlation and all 25
keyboard routes pass. The live adapter and human device review remain. None of this makes online racing
available before A3 `GO`.

| View | Primary | Cancel | Bounded timeout outcome |
|---|---|---|---|
| Online entry | Create Private Room | Back | Not a wait |
| Creating/joining | Connection Details | Cancel | Try Again |
| Room | Share Invite | Leave Room | Not a network wait |
| Preflight | Review Checks | Leave Room | Retry Checks |
| Selecting | Choose Character → Choose Vehicle → Ready | Leave Room | Not a network wait |
| Loading/rematch | Connection Details | Cancel to Lobby | Return to Lobby |
| Countdown | Connection Details | Cancel to Lobby | Return to Lobby |
| Racing | Connection Details | Leave Race | Not a wait; leave confirms policy |
| Results | Race Again | Return Home | Not a wait |
| Recovery | Exact next action | Return Home | Play Here |

An unrecognized adapter failure maps to **Could Not Reach the Room** and a
bounded local diagnostic code outside this model. Recovery is assertive;
ordinary progress is polite and never steals focus. `race_admission_enabled` is
local release configuration, never service data, and Start also requires a
leader plus at least 2 fully ready members. It remains false before A3 `GO`.

Preflight rows use one of **Ready**, **Needs update**, **Different ROM version**,
**Different gameplay settings**, **Controller needed**, or **Connection check
failed**. Every failed row has one owner and one action; the host cannot override
compatibility. The room link and code are role-scoped and rotation invalidates
the old invite.

Lobby member grouping is by endpoint, then local seats. Host is a responsibility
label, not a special racer. Show **Direct connection** or **Limited connection**;
expose raw path, RTT, jitter, loss, rollback depth and build ids only under
**Connection details** with a Copy diagnostics action that excludes secrets,
names, addresses, SDP and input.

Barrier copy is explicit:

| Barrier | Progress text | Timeout recovery |
|---|---|---|
| Vote | Choosing track — 2 of 3 ready | Change vote / Leave |
| Load | Loading race — Alex is still loading | Retry that endpoint / Cancel to lobby |
| Countdown | Starting together… | Return to lobby on epoch mismatch |
| Results confirmation | Confirming race results… | Keep local result visible; do not write progression |
| Rematch | Preparing race again… | Return to lobby; room remains intact |

The online overlay never pauses simulation. Its actions capture navigation and
show connection/controller/leave panels. Gameplay-changing settings say **Will
apply after this race**. Leaving requires confirmation that names whether
deterministic AI will take over or the race will end under current room policy.

## Error catalog

| Stable id | Player copy | Primary recovery |
|---|---|---|
| `invite_expired` | That invite expired. Show a new code on the display. | Enter another code |
| `invite_rotated` | The display replaced that invite. Scan the newest QR code. | Enter another code |
| `left_room` | This phone no longer controls a racer. | Enter another code |
| `room_full` | All four phone controller seats are in use. | Return |
| `pending_full` | Too many phones are waiting. Ask the display to decline one. | Try again |
| `approval_rejected` | The display declined this phone. | Enter another code |
| `host_closed` | The display ended the phone controller room. | Enter another code |
| `room_expired` | This controller room ended. | Enter another code |
| `protocol_update_required` | This controller page needs an update. | Refresh controller |
| `duplicate_controller` | This controller is already open. | Use this tab |
| `seat_reclaimed` | The display reassigned this controller seat. | Enter another code |
| `rate_limited` | Too many code attempts. Wait a few minutes, then try again. | Return |
| `service_budget_safe` | Phone pairing is full right now. Local controllers still work. | Play here |
| `transport_lost` | Connection lost. Controls were safely released. | Reconnect / Leave |
| `service_unavailable` | Could not reach the controller room. Check your connection. | Try again |
| `different_build` | Everyone needs the same game version. | Update / Leave room |
| `different_rom` | Everyone needs the same supported ROM revision. | Choose ROM / Leave room |
| `different_settings` | Gameplay settings do not match the room. | Use room settings |

Never display “unknown error.” Unrecognized ids map to `service_unavailable`,
retain a bounded local diagnostic code, and offer the stable local path.

## Accessibility and responsive contract

- Use native buttons, inputs and dialogs where possible; every icon action has
  a visible or accessible text name. Heading order describes state hierarchy.
- Visible focus meets the component boundary and is never removed. Keyboard,
  switch and gamepad order follows visual order without positive `tabindex`.
- Status updates do not steal focus. `role=status` is polite; safety release,
  expiry and action failures use an assertive but non-repeating alert.
- Touch actions prefer 44×44 CSS px and never fall below 24×24 with 8 px target
  separation. Controller comfort is approved by physical-hand tests, not size
  math alone.
- Support 320×568 CSS px, portrait/landscape, safe-area insets, 200% text and
  reflow without two-dimensional page scrolling. QR stays fully visible or the
  code/instructions remain immediately adjacent.
- Contrast is at least 4.5:1 for ordinary text and 3:1 for large text/controls.
  Increased contrast adds borders and text marks; reduced transparency uses
  opaque panels. Do not encode state in color alone.
- Screen reader announcements say controller number and source before state.
  The analog surface has concise instructions, while individual gameplay touch
  targets expose pressed state without flooding announcements during steering.
- Both launcher and phone analog surfaces are focusable: Arrow keys or WASD
  steer, diagonals stay inside the N64 analog radius, visible and spoken
  direction update together, and key release, blur, page hide or connection
  loss synchronously returns the stick to exact neutral.

## Motion and feedback

Launcher dialogs use one short opacity/scale transition tied to the trigger;
they remain interruptible and reverse when dismissed. Default easing is
critically damped—no bounce, confetti or decorative delay in setup/error paths.
Reduced motion uses a ≤100 ms cross-fade or no transition. Controller buttons
and stick position update in the same animation frame as pointer input and are
never network-acknowledgement animations.

Celebrate only durable achievements: first successful controller test, room
ready and confirmed race completion. Celebration cannot cover controls, delay
the next action, use haptics when disabled, or run under reduced motion.

## UX evidence template

Each milestone records build/commit, OS/browser/device, viewport/text scale,
input methods, assistive technology, journey completion, time-to-first-control,
mis-scan/wrong-role/backtrack counts, exact observed error recovery, motion
preference and notes from chord comfort. A human-study row distinguishes
observation from interpretation and lists resulting copy/layout changes.

Automation owns state/focus/layout bounds and negative controls. It cannot sign
off camera scanning in installed browsers, thumb comfort, vibration feel,
screen-reader quality or comprehension; those rows require named physical-device
review before A1/A2 release.
