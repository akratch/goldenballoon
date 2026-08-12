# A0 session foundation evidence

Decision: **OPEN — automated evidence review; no `GO` yet.**

The v2 bridge exposes a transport-neutral local-input drain at the
launcher/engine boundary. Its focused test maps a two-seat endpoint onto
canonical slots 0 and 2, preserves remote slots 1 and 3 byte-for-byte, adds
rather than replaces confirmation bits, and rejects count or analog violations
without partially mutating bridge state. The concrete launcher-owned
`MdkrMatchTransport` now consumes that seam: it owns canonical repeat-last
history, admits remote samples only under an authenticated peer slot mask,
rejects local-slot injection and exact-epoch/window/conflict failures, commits
local seats atomically, and exposes the earliest corrected tick. Native
`SessionRuntime` creates and retires it with the admitted engine epoch. This is
The engine consumes that drain through a versioned, engine-lifetime copy-out C
provider. Four isolated processes send scripted remote seats through the
launcher adapter; one rendered endpoint withholds a real A-edge for four ticks,
rewinds five authored ticks, and converges after replay. This remains
not evidence that a browser/native carrier, authentication handshake, relay or
online race exists yet.

| Field | Evidence |
|---|---|
| Revision/build | Record at acceptance run |
| Automated lifecycle | Native production shell: both three short engine epochs and three complete 8,200-frame 4P races/two rematches, one session id, launcher draw between loans and three clean teardowns; four isolated online-layout processes enter through exact-epoch launcher envelopes and retire their copied rosters cleanly; browser: persistent wasm repeat play; session core/bridge/runtime CTest |
| Negative controls | Stale generation, invalid transition/manifest, missing/future/stale match epoch, partial core/bridge phase advance, remote viewport before engine admission, second wasm main/module, online pause request |
| Human/accessibility matrix | OPEN |
| Reviewer/date | OPEN |
| Decision and expiry | OPEN |
