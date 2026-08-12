# A1/A2 Phone Party evidence

Decision: **OPEN — browser direct-path automation passes; device/release gates remain.**

| Field | Evidence |
|---|---|
| Service/QR | Party `npm run check`; QR ECC-Q decode; pinned dependency/license gate |
| Browser UI | Controller and host Chrome gates at 320×568 and 200% text; rapid close/reopen cannot destroy the new room. Leave/end are separately confirmed with safe default focus, cancel is non-mutating and voluntary leave has truthful neutral recovery copy. Standalone room handoff has skip focus and notch-safe layout. Both analog surfaces expose visible-focus Arrow/WASD steering, bounded diagonals, spoken direction and exact-neutral release/blur behavior. |
| Direct path | Two-process WebRTC state/control/input-test/haptics gate |
| Engine handoff | Remote fail-neutral plus independent P1–P4 wasm queue gate; in-game setup pauses/neutralizes local input; reconnect reservation blocks local takeover until explicit release |
| Seat UX | Keyboard/gamepad/touch/phone labels, recommended free slot, explicit labeled replacement and host-selected seat request pass Chromium automation |
| Security controls | Origin/body/key/rotation/replay/rate/census tests; 20-bit phrase derivation; sheet dismissal revokes its QR/code but preserves approved leases |
| Four physical phones/mixed race | OPEN |
| iOS/Android/accessibility/hand comfort | Automated keyboard/switch-compatible steering, focus and semantics pass; physical VoiceOver/TalkBack and hand-comfort review OPEN |
| Native macOS/Windows/Linux | OPEN (A2) |
| Cost/deploy/privacy review | Local dry-run only; production OPEN |
| Reviewer/date/decision | OPEN |
