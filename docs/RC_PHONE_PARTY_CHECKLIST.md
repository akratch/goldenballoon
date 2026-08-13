# Phone Party — RC blessing checklist

This is the complete list of Phone Party exit items that **cannot** be closed by
automation. Everything else in the A1/A2 exit lists is closed by gates in this
repository (see [`docs/multiplayer/STATUS.md`](multiplayer/STATUS.md)) or is prepared to
one command (see [`docs/multiplayer/DEPLOY_PHONE_PARTY.md`](multiplayer/DEPLOY_PHONE_PARTY.md)).

Every item below needs a human holding physical hardware. Each states the **exact
expected observable** — bless the RC only when you have seen that observable, not merely
"it seemed to work". Record what you saw under `docs/evidence/multiplayer/` with the
build version, device model and OS/browser version.

## Preconditions

Before starting, all of these must already be true:

- [ ] The Party service is deployed and `python3 tools/verify_party_deploy.py --origin https://<your-party-origin>` printed `PASS`.
- [ ] The RC build was compiled with `-DMDKR_PARTY_ORIGIN=https://<your-party-origin>` — the **same** origin the verifier passed against. A build pointing at the placeholder `.invalid` origin cannot pair and must not be blessed.
- [ ] **Confirm the surface is there at all.** A build compiled with no party origin deliberately shows *no* Phone Party surface — no launcher card, no in-game overlay entry. So "I can't find Phone Party" means the RC was built without an origin, not that the feature broke. Open the launcher's **Play** panel with a ROM loaded and confirm the **Use a Phone as a Controller** card is present before starting anything below.
- [ ] The release workflow for this exact commit is green, including the party-host bring-up smoke and the binary-size gate on all three platforms.

---

## 1. Solo phone controller (single-player)

**Do:** From a cold launcher with a ROM loaded, open **Play → Use a Phone as a
Controller** and pair ONE phone by QR. Approve it (it defaults to Controller 1). With no
keyboard or gamepad touched, start an ordinary single-player Adventure/race and play with
the phone alone.

**Expected observable:**
- The single approved phone drives player one: steering, accelerate, brake, item and horn
  all respond, with no perceptible lag versus a wired pad.
- A connected-but-idle desktop keyboard/gamepad does not fight the phone for player one
  (the phone holds the seat); unpairing the phone returns control to the keyboard.
- Nothing about the flow implies a second player is required — a solo player can pair one
  phone and play the whole game with it.

## 2. Four-phone race

**Do:** From a cold launcher with a ROM loaded, open **Play → Use a Phone as a
Controller** and pair four different phones by QR. Approve each into a distinct seat. Start
a four-player standard race and run it to the results screen. Mid-race, open the in-game
overlay's compact Phone Party entry and confirm it lists the same four seats.

**Expected observable:**
- Four distinct seats are occupied, each phone's on-screen seat label matches the kart it
  drives — verify by having each player accelerate alone, in turn, and confirming only
  their kart moves.
- Steering, accelerate, brake, item and horn all respond with no perceptible lag
  (indistinguishable from a wired pad at arm's length).
- The race completes to results with four finishing positions and no kart freezing,
  teleporting, or reverting to AI mid-race.
- No phone drops, reconnects, or shows a recovery banner at any point during the race.

## 3. iOS Safari pairing

**Do:** On an iPhone running current iOS, open the stock Camera app, scan the launcher's
QR code, and follow the link into Safari. Pair and take a seat.

**Expected observable:**
- The camera's link banner opens Safari directly to the controller page over **HTTPS** —
  no certificate warning, no "not secure" chip, no App Store or install prompt.
- The comparison phrase shown on the phone is **character-for-character identical** to
  the phrase shown in the launcher. Approve only on a match.
- The controller renders full-bleed with the D-pad/stick and buttons clear of the notch,
  the home indicator and the Dynamic Island, in both the initial orientation and after
  rotation.
- Input reaches the game within one screen refresh of the touch.

## 4. Android Chrome pairing

**Do:** On an Android phone running current Chrome, scan the same QR with the stock
camera or Chrome's scanner. Pair and take a seat.

**Expected observable:**
- Same as item 3: HTTPS with no warning, no install prompt, phrase matches exactly.
- Also verify the **fallback code** path on this device: dismiss the QR, enter the
  six-digit code shown by the launcher on the controller page, and confirm it lands in
  the same room. Then rotate the invite in the launcher and confirm the **old** code is
  refused with a neutral message, and the **new** code works.

## 5. Real-phone lifecycle: rotation, background, sleep/wake, network change

Run all four on at least one iOS and one Android device, with the phone **already racing
in a seat**.

**Do (rotation):** Rotate the phone portrait → landscape → portrait mid-race.
**Expected observable:** Controls reflow without losing the seat; no reload; no input
gap longer than the rotation animation; the kart does not veer from a stuck input.

**Do (background):** Switch to another app for ~30 seconds, then return.
**Expected observable:** While backgrounded, the kart **coasts in neutral** — it does not
hold the last input and does not hand control to AI or another player. On return, the
same seat is resumed automatically with no re-scan and no re-approval.

**Do (sleep/wake):** Lock the phone for ~60 seconds, then unlock.
**Expected observable:** Same as background — neutral while asleep, same seat on wake.
If the page was evicted, the phone shows a neutral reconnect state and a single tap
restores the **same** seat; it never silently joins a different seat.

**Do (network change):** With the phone paired and racing, switch Wi-Fi → cellular (or
toggle airplane mode for ~10 seconds and back).
**Expected observable:** Established direct controls keep working through a brief
room-service interruption. If the direct channel does drop, the phone shows the neutral
recovery state, the kart coasts, and reconnect returns the **same** seat. Meanwhile the
game itself never stalls, and local keyboard/gamepad players are unaffected.

## 6. Firewall / blocked-network observation

**Do:** Put the host machine behind a restrictive network (guest Wi-Fi with client
isolation, or block outbound UDP), then attempt to pair.

**Expected observable:** The launcher shows the documented fail-neutral message rather
than hanging or crashing; **Play Here** and local keyboard/gamepad play remain fully
usable throughout. The automated gate proves the code path; this item confirms a real
restrictive network produces the same message and the same recovery.

## 7. Haptics feel

**Do:** With haptics enabled on the phone, race a lap that includes an item hit, a wall
collision and a boost.

**Expected observable:** Vibration fires on those events, is short and distinct (not a
continuous buzz), and is not perceptibly late relative to what is on screen. Toggling
the phone's local haptics opt-out **immediately** stops all vibration and does not
disturb the seat or any other player. A phone that reports no vibration support pairs and
plays normally with no error.

## 8. Signing and notarization

**Do:** Sign and notarize the macOS RC; sign the Windows RC. Then download each artifact
the way a player would — through a browser, from the release page — onto a machine that
has never seen this build.

**Expected observable:**
- macOS: the app opens on first double-click with **no** Gatekeeper "unidentified
  developer" or "damaged" dialog. `spctl -a -vvv` reports `accepted` / `Notarized
  Developer ID`, and `xcrun stapler validate` succeeds on the downloaded artifact.
- Windows: SmartScreen does **not** show the "unrecognized app" blocking dialog on the
  downloaded `.exe`; the signature shows a valid publisher in the file's Properties →
  Digital Signatures tab.
- On both, after that clean install, Phone Party pairs a phone successfully — signing
  must not break the compiled party origin or the transport.

---

## Blessing

Bless the RC only when every box above is checked with its stated observable seen on real
hardware. Any box that cannot be checked names the failing boundary; record it and do not
ship Phone Party as GA until it is closed or the scope is explicitly reduced in the
player-facing notes.
