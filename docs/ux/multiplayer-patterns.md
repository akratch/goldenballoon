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

For Phone Party, **Limited connection** has one precise meaning: both direct
DataChannels are healthy but the room/signaling socket is retrying. Keep the
active controller visible and usable, show the gold connection mark, and make
one polite announcement that direct controls remain connected. Do not show a
spinner over gameplay or release input for signaling alone. If either direct
channel or the peer path fails, immediately publish neutral, show
**Reconnecting — Controls are safely released**, preserve the numbered seat,
and rebind with a fresh peer generation. When signaling recovery rotates the
connection epoch, neutralize before the rebind; never accept packets from the
old epoch. Recovery announcements do not steal focus. Host and phone signaling
each stop after five consecutive or short-lived attempts; 30 stable seconds
reset the sequence. A phone without a healthy direct path then exposes **Try
now**. If the direct path is healthy, play continues with **Limited connection**
even after automatic signaling retry pauses.

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

Focus does not jump when a tile changes state. **Remove** opens a native
confirmation that names the phone and numbered seat, describes immediate
neutral input, and initially focuses the safe **Keep phone** action. Cancel
performs no request and returns focus to the same **Remove** action. Confirmed
removal returns contextual focus to the now-**Available** seat tile; if another
host removes it while confirmation is open, the dialog closes, focuses that
same tile and announces that no further action was needed. Newly pending phones
are announced politely; errors and controller release use assertive announcements.

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
The launcher erases the QR bitmap, grouped code and controller URL from its
model and DOM before waiting for the revoke request. The exact revoke response
advances local invite-generation custody; reopening waits for that correlation
before rotating, so rapid close/reopen cannot submit a stale generation.
**Start local game** uses this same erase/revoke path before it clicks Play;
approved phones persist and the launcher no longer advertises the old invite.
Remote revocation proceeds immediately but never blocks offline local play; if
the request cannot complete, the original two-minute service TTL remains the
hard ceiling. The controller service does not mirror a gameplay/race phase—
that state remains in the launcher and session boundary.
Ending requires a separate confirmation whose initial focus is **Keep
controller room** and which names the disconnected phones plus surviving local
inputs. A later open rotates to a fresh QR/code. Focus returns to the invoking launcher
or in-game control. Starting the game moves focus to the game canvas instead of
a hidden launcher action.

One displayed QR intentionally admits multiple friends during its two-minute
window. Each phone appears independently and cannot send input before approval.
**Extend** rotates both QR and code and says: **Invite extended. The previous QR
code is no longer valid.**

The service returns the actual remaining invite lifetime, capped by the room's
deadline. The launcher starts a receipt-relative countdown up to 5% (at most
ten seconds) early, so network delay cannot make a stale QR look usable. QR
generation is an enhancement: if the offline encoder or canvas fails, hide the
canvas, say **Open this site's /controller/ page**, and keep the grouped and
individually announced six-digit code as the complete recovery path.
At countdown expiry, apply the same immediate erasure and replace the content
with **Invite expired — approved phones stay connected** plus **Show a new QR
code**; never leave an expired capability visible while merely relabelling it.

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
| Reconnecting | Reconnecting / Controls are safely released | Try now | Leave |
| Duplicate | This controller is already open in another tab | Use this tab | Leave |
| Terminal | Exact reason | Enter another code | Return to instructions |

The phone consumes only an exact query-free `/controller/#…` invitation and
scrubs it before configuration or feature checks. Scrub failure abandons the
join at clean `/controller/` code entry. The in-memory capability is never
written to web storage. Embedded or unsupported browser recovery truthfully
offers **Share private link** with **Copy private link** as its fallback while
the pre-redemption capability exists. Without one it offers **Share controller
page** / **Copy controller page**, then tells the player to enter the current
code in Safari or Chrome. Each URL is built only for that user gesture.
**Use this tab** rebuilds the private URL only
after the old tab acknowledges neutral/close and an ordinary exclusive lock is
available, for one same-tab navigation whose new document scrubs it again. If
coordination cannot prove single ownership, the action stays put and tells the
player to close the other tab before retrying.
Copying the already-clean address or reloading it would strand the player and is
therefore not an acceptable recovery.

Protocol mismatch uses a real **Refresh controller** action that reloads the
static controller client. Other typed terminal failures use code entry only
when the current browser can actually recover that way; action labels never sit
over a generic handler with different behavior.

Leave from Waiting, Assigned, Active or Reconnecting always opens a confirmation
focused on **Keep controller**. Nothing neutralizes or disconnects until **Leave
controller room** is chosen. The resulting terminal state says **Controller
disconnected** rather than mislabeling a voluntary leave as an expired invite.

The controller is a control surface, not a miniature game UI. The analog stick
tracks the captured pointer 1:1; button feedback begins on pointer-down. Each
pointer owns one control until release/cancel. `pointercancel`, visibility loss,
page hide, direct-channel loss, overflow and leave all publish exact neutral.
Signaling-only loss is the exception because the authenticated direct path
remains healthy; it changes status, not pad state. A
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
local release policy. It emits title, explanation, calm status, an optional
strictly bounded three-compound verification phrase, primary/secondary/Cancel
controls, timeout recovery, announcement priority and bounded counts. UI
adapters render this model and dispatch its typed actions; they do not invent
copy or infer room transitions from provider callbacks.

The native launcher binding lives in `platform/app/ui_online_room.*`. With no
test flag it renders an honest unavailable state and performs no network work;
`MDKR_APP_ONLINE_FAKE=1` is an offline design/evidence adapter only. Its 43-case
gallery covers every view and typed failure, its speech walk enumerates every
title/action set, and all 27 public actions have typed keyboard/gamepad routes.
The browser launcher has the same honest zero-I/O production entry and
hands local recovery to the existing ROM and Phone Party owners. Its explicit
evidence adapter renders a standalone sub-128-KiB Wasm projection compiled from the
same C reducer/view model; JavaScript owns DOM semantics and launcher routes,
never room transitions. Its pure semantic presenter is also exercised in Node
against JSON emitted by all 43 authoritative C projections, covering action and
selection order, timeout priority, count grammar, local recovery substitution,
immutable output, malformed input, phrase copy and announcement priority without
claiming browser layout or accessibility-tree evidence. That presenter also owns
the exhaustive live-action admission table: only a currently rendered, enabled
action can reach a launcher effect, and a carrier/race-dependent action produces
specific locked guidance without network work. Gallery selection remains a test
capability, never a production navigation API. The 43-case browser/native gate now requires identical
phrase copy, semantic grouping, `translate=no`, announcement text, explicit
**Words Match** / **Words Differ** decisions and all 27 keyboard routes. Its prior 25-action rendered
baseline passed; rerunning the expanded rendered gate and human device review
remain. None of this makes online racing available before A3 `GO`.

Live service data crosses a separate pure boundary before the presenter. That
boundary accepts only the documented public MatchRoom shapes, copies them into
one deeply immutable launcher schema and keeps the bearer/opaque room identity
stable across state-only publications. It rejects private reducer receipts,
unknown fields, identity substitution, non-contiguous control history,
impossible seat/phase state and cross-origin invitations. Invitation custody is
generation-bound: a publication that advances the generation without the new
secret immediately removes the old Share action. The state and invite commit as
one transaction only after native projection and semantic rendering succeed.
For guest Leave, an accepted response is the terminal success state—the UI does
not ask the server to authenticate a credential that Leave just revoked.

The state socket may beat an HTTP response. Fully valid older public snapshots
are ignored rather than turning a successful action into an outage, while mixed
regression/advancement and equal-revision changes still fail closed. When a
membership update beats a rotate response, the launcher may attach that
response's secret to the newer room only if it names the exact generation
expected by that in-flight rotation, the already-current invite generation and
the current local leader; it never rolls member state back, extends locally
expired custody or revives an older link.

Invite expiry is a local security transition, not a passive timestamp label.
The launcher derives a conservative receipt-relative deadline rather than
trusting the display wall clock. At expiry it removes the QR, link and code from
memory and the DOM before reprojecting. A leader sees **Invitation Expired**
with one **New Invitation** action; a guest never receives rotation custody.
Friends already in the room remain connected, and the copy says so while
replacement is in progress. Projection failure may preserve the last public
room state, but it must never restore the expired secret.

QR rendering is an enhancement over the same invitation, not a single point of
failure. If the offline QR module is missing or rejects the payload, the empty
canvas is not exposed as an image; the accessible six-digit code and bounded
Share action remain, with copy that no longer tells the player to scan.

Pasted invitations accept only a six-digit ASCII code, a raw 43-character
capability, or an exact same-origin `/room/#match=…` URL. Lookalike origins,
embedded credentials, query strings, alternate paths, encoded capabilities,
extra fragment fields and control-character whitespace fail before room I/O.
The `/room/` handoff keeps the capability in the fragment for the one
same-origin redirect, then the always-loaded launcher captures it only in
closure memory and erases the address synchronously before policy/model/ROM
work. It never uses session or local storage. If URL scrubbing fails, the page
navigates to the clean route and abandons the join instead of continuing with
the capability. A disabled build discards the
capability immediately and opens a calm **Private Rooms Aren’t Enabled in This
Build** recovery with local play preserved. An enabled build that still needs a
ROM names that requirement and retains the in-memory handoff for at most the
ten-minute protocol TTL; timeout erases it and asks for a new link or current
room code.

**Room Ended** and **Room Expired** are terminal local states. Once either is
projected, the launcher closes its subscription and discards the room bearer;
**Return Home** and **Play Here** never wait on or retry a server that has
already ended the room. The recovery view remains visible long enough to make
the reason clear, but it contains no live capability material.

State-socket recovery is deliberately finite. Consecutive or short-lived
connections use five launcher-owned backoff attempts; only 30 seconds of stable
custody resets that counter. Exhaustion stops automatic reserve use and shows
the normal **Try Again** recovery. An explicit Retry refreshes authenticated
state first, resets the bounded sequence and then reconnects. A synchronous
adapter/WebSocket setup failure uses this same path; it never becomes an
unhandled page error or leaves a blank/stale room view.

| View | Primary / secondary | Cancel | Bounded timeout outcome |
|---|---|---|---|
| Online entry | Create Private Room | Back | Not a wait |
| Creating/joining | Connection Details | Cancel | Try Again |
| Room | Share Invite; after expiry, New Invitation (leader only) | Leave Room | Not a network wait |
| Preflight checks | Review Checks | Leave Room | Retry Checks |
| Phrase comparison | Words Match / Words Differ | Leave Room | Human-paced; no timeout |
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
| `seat_reclaimed` | The display removed this phone controller. Its controls are neutral. | Enter another code |
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
