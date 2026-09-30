# Issue intake and classification

Golden Balloon is a native port. Use the Bug report template for incorrect
existing behavior and Feature request for new capabilities. The latter uses
the existing `enhancement` label ("New feature or request"), not `bug`.
Parity report remains available for differences from original/reference behavior.

This is a local template/policy preparation, not a published GitHub change.
Templates become available only after merging into the default branch; see
[GitHub's template documentation](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/configuring-issue-templates-for-your-repository).

## Maintainer triage

### Read-only GitHub refresh, 2026-09-27

The open issue inventory remains #58, #65, #66, #67 and #68. The issues API
also returns pull requests; these are tracked separately, not counted as two
additional bug reports:

- [PR #64](https://github.com/akratch/goldenballoon/pull/64): grouped GitHub
  Actions major upgrades across five workflows. Review runner compatibility,
  artifact upload/download interoperability, checkout security behavior and
  pinned revisions before adopting; local engine CTest cannot qualify CI
  action upgrades. No merge or workflow dispatch was performed.
- [PR #1](https://github.com/akratch/goldenballoon/pull/1): older post-1.0
  integration proposal, head `4d7d3bb6ac2d4920126f9d5be18887cf5408f4d7`.
  Reconcile against current integration before deciding its disposition; do
  not merge or close it solely because the current tree passes local tests.

The source-available [beta.11 branch](https://github.com/ThatGuyMcd/DKR-R/tree/build/1.0.5-beta.11)
still resolves to `4ddfa1405eb76e8c073ea02603c5d557725376cf`.
The [additional build branch comparison](https://github.com/ThatGuyMcd/DKR-R/compare/4ddfa1405eb76e8c073ea02603c5d557725376cf...3d281438ba0d69a166c8262010822a8ffb4e690e)
shows CI/toolkit changes only; it does not add a new runtime parity requirement.
All existing feature and report acceptance requirements remain open unless
their own evidence says otherwise. No GitHub state was mutated.

### Read-only GitHub refresh, 2026-09-23

Open inventory remains #58 (portraits/NDS additions), #65 (custom tracks),
#66 (texture-pack stutters plus Enhanced audio), #67 (finish-banner clipping),
and #68 (feature-request classification). All five currently carry `bug`;
`enhancement` already exists with description "New feature or request". No
issue or label was mutated. Issue #66 has no comments with additional evidence.
Do not close the mixed #58/#66 requests based on only one subfeature or symptom.

DKR-R `build/1.0.5-beta.11` remains pinned at
`4ddfa1405eb76e8c073ea02603c5d557725376cf`; current `main` is
`8a8e927e9ea14c5c07ca7ad74b44fdbe077f5ff6`. GitHub compare reports main behind
that beta branch by two commits, with none ahead. The reviewed prerelease
therefore remains the source target for the custom-track parity backlog.

### Release gate: Windows single controller unexpectedly joins two players

Status: open investigation; not classified as user error. Report lacks exact
build, controller and input-stack details. Ordinary launcher boot does not
carry a player-count setting. Title-screen controller assignment resets active
players to one; character select admits another seat on that seat's A/Start.
Keyboard presence alone occupies P1, not an extra player.

Maintainer follow-up: reporter uses Windows described as "latest", with
keyboard, mouse and one controller connected, and cannot enter single-player.
No further details are available to the maintainer. Do not interpret "latest"
as a verified Golden Balloon version or ask repeatedly for unavailable details.
Explicitly include keyboard+mouse+one-pad coexistence in Windows reproduction;
their simultaneous presence should not force multiplayer.

Added local coexistence coverage: hotplug fixture injects keyboard Return via
SDL events while one attached pad is neutral, releases it, then resumes pad
input. Game-facing P1 receives Start while P2–P4 remain absent and neutral;
existing pad takeover/reconnect assertions still pass (Release, ten assertions).
This is not mouse-input or Windows hardware/end-to-end menu qualification.

Further coexistence check holds keyboard Start while controller A takes over
P1; P2–P4 remain absent/neutral. Both Release and ASan/UBSan pass eleven
assertions, including explicit acceptance of keyboard press/release by SDL.
Source review finds no mouse-to-controller-port mapping in the engine event
dispatcher. This does not exclude a Windows remapper producing extra devices,
nor certify launcher-to-character-select behavior on that platform.

Local end-to-end follow-up: a 2000-frame keyboard-Start/one-pad route reaches
character select (menu 3) and the single-player caution screen (menu 28).
Trace records title assignment to P1 with players=1 and no subsequent join;
P2–P4 stay absent/neutral. Added this as an explicit controller gate arm. This
validates the engine menu journey locally, not the Windows launcher/hardware
path; no controller filtering or player-count behavior was changed.

Positive control now adds a second virtual controller at character select and
requires exactly one P2 join with players=2. The single-player oracle rejects
that same run, proving it detects the unwanted-join symptom rather than merely
missing menu activity. All thirteen assertions pass in Release and ASan/UBSan,
including both menu routes. These are local SDL virtual-device tests; physical
Windows devices/remappers remain outside their scope.

The historic startup-enumeration/device-added double-open bug was fixed in
7b65bd7c, an ancestor of the public 1.7.0 base 106bad37. Current gc_try_open
rejects repeated instance IDs and repeated opened controller pointers. It does
not establish physical identity across distinct SDL devices; physical plus
virtual endpoints remain a hypothesis requiring Windows evidence. Do not
deduplicate by controller name/model GUID: two genuine pads may share those.

Local Release and ASan/UBSan virtual-controller hotplug checks each pass eight
assertions, including a boot-attached pad owning exactly one port despite the
queued add event. This is macOS SDL-path evidence, not Windows qualification
or proof that one A/Start cannot be duplicated by a Windows input stack.

Follow-up: extended the boot-attached virtual-pad route with separate A and
Start presses before any second device exists. Both current local builds pass
the new ninth assertion: P1 receives the expected button while P2–P4 remain
absent and neutral. This observes game-facing ports, not Windows device
enumeration or an end-to-end character-select player-count transition.

Required before qualifying Windows release:

Assignment diagnostics now include the SDL connection instance ID and device
GUID alongside the player port and name. No serial numbers or device paths are
logged. GUID is a device-class clue, not proof of physical identity. The local
Release hotplug gate passes ten assertions, including exactly one logged P1
assignment for the boot controller. Windows hardware qualification remains open;
this logging change does not filter devices or claim to fix the report.

- Record exact packaged build, controller model/connection, direct versus Steam
  launch, and any input remapping software. Obtain the launch log's SDL gamepad
  assignment lines and distinguish two occupied character slots from a later
  two-player race or an Adventure Party/Joint Venture setting.
- Exercise one pad connected before launch and added afterward; one A/Start
  must control/join one seat. Verify keyboard use does not create another seat.
- Exercise reconnect and two genuine controllers; do not fix the single-pad
  case by preventing legitimate two-player participation.
- If two SDL identities are observed for one physical pad, reproduce with and
  without the reported remapping path and scope a source-identity solution.
  If only one identity is present, trace per-port button events and game joins.

Maintainer information request sent; keep open until Windows reproduction or
an evidence-backed disposition. Do not close from the local hotplug pass alone.

- Separate reproduction evidence from suspected causes. An experimental mode
  can still have a real bug; a failed reproduction is not proof of absence.
- Record exact build, platform, settings and reproduction path when known.
  Request only missing details and keep work that can proceed locally moving.
- For mixed issues, track each bug and feature independently. Do not close the
  whole request because one part passes. Splitting or relabeling public issues
  requires maintainer approval in this work session.
- Source-available DKR-R prereleases are valid investigation inputs. Pin the
  source revision and distinguish implementation, synthetic tests, actual-content
  acceptance and packaged-platform acceptance. Upstream support alone is not
  native-port qualification.
- Close with evidence tied to a build or an explicitly agreed disposition,
  not to meet an issue-count target. Do not request public ROM-derived artifacts.

## Prepared dispositions (not applied)

- #65 custom tracks: feature; propose `enhancement` instead of `bug`.
- #58 portraits plus Dixie/Tiny and DS content: mixed; preserve the unresolved
  portrait report while separately tracking character and track requests.
- #68 feature-request label: the chooser now has a prepared Feature request
  template mapped to `enhancement`. This does **not** create the literal
  `feature-request` label requested by the reporter. Maintainer must decide
  whether the existing label is sufficient or approve creating a dedicated one
  and updating the template. Keep the issue open until that disposition and
  publication are verified.
- #66 texture stutter/Enhanced audio and #67 Windows finish-banner clipping:
  retain bug classification while investigating.

After authorized publication, verify the public chooser, prefilled feature
label and unchanged bug route. Local frontmatter checks cannot prove GitHub's
rendered behavior or permission to create a label.
