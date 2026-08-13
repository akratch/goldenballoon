import assert from "node:assert/strict";
import {createRequire} from "node:module";
import {fileURLToPath} from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const boundary = require(path.join(
  root, "dist/web/online/online-room-live-state.js"));

const origin = "https://play.example";
const now = 1_900_000_000_000;
const roomName = "abcdefghijklmnopqrstuv";
const credential = "C".repeat(43);
const compatibility = Object.freeze({
  protocolVersion: 1,
  buildId: Object.freeze(Array.from({length: 16}, (_, index) => index + 1)),
  gameplayDigest: Object.freeze(
    Array.from({length: 32}, (_, index) => 128 + index)),
  romRevision: 1,
  cadenceHz: 30,
});

function member(endpointId, overrides = {}) {
  return {endpointId, seatCount: 1, connected: true, ready: false,
    loaded: false, ...overrides};
}

function seat(endpointId, overrides = {}) {
  return {endpointId, selectionRevision: 0, voteTrack: null, localIndex: 0,
    characterId: null, vehicleId: null, ...overrides};
}

function snapshot(overrides = {}) {
  const {lobby: lobbyOverrides = {}, ...rootOverrides} = overrides;
  const lobby = {protocolVersion: 1, revision: 1, matchEpoch: 0,
    leaderGeneration: 1, roomId: "42", leaderEndpointId: "100",
    phase: "lobby", compatibility: structuredClone(compatibility),
    members: [member("100")], seats: [seat("100")], selectedTrack: null,
    selectedVehicleMask: 0, ...lobbyOverrides};
  return {type: "match_state", schemaVersion: 1,
    expiresAt: 2_000_000_000_000, inviteExpiresAt: 1_999_999_000_000,
    inviteGeneration: 1, closedReason: null, lobby, controlTail: [],
    ...rootOverrides};
}

function createWire(overrides = {}) {
  return {...snapshot(), roomId: roomName, endpointId: "100", credential,
    fallbackCode: "123456", inviteExpiresInMs: 600_000,
    inviteUrl: `${origin}/room/#match=${"A".repeat(43)}`, ...overrides};
}

function changed(value, mutate) {
  const copy = structuredClone(value);
  mutate(copy);
  return copy;
}

function rejected(value, priorState = null, priorInvite = null,
                  expected = compatibility, expectedOrigin = origin,
                  receiptNow = now) {
  assert.equal(boundary.ingest(value, priorState, priorInvite,
    expected, expectedOrigin, receiptNow), null);
}

assert.equal(boundary.version, 1);
assert.equal(boundary.validCompatibility(compatibility), true);
assert.equal(boundary.sameCompatibility(compatibility,
  structuredClone(compatibility)), true);
assert.equal(boundary.validCompatibility({...compatibility, extra: true}), false);
assert.equal(boundary.sameCompatibility(compatibility,
  {...structuredClone(compatibility), cadenceHz: 25}), false);

const inviteCapability = "A".repeat(43);
assert.deepEqual(boundary.parseInvite("123456", origin), {code: "123456"});
assert.deepEqual(boundary.parseInvite("123 456", origin), {code: "123456"});
assert.deepEqual(boundary.parseInvite(inviteCapability, origin),
  {capability: inviteCapability});
assert.deepEqual(boundary.parseInvite(
  `${origin}/room/#match=${inviteCapability}`, origin),
{capability: inviteCapability});
for (const invalid of ["123\t456", "123  456", "１２３４５６",
  `https://evil.example/room/#match=${inviteCapability}`,
  `${origin}/controller/#match=${inviteCapability}`,
  `${origin}/room/?from=mail#match=${inviteCapability}`,
  `${origin}/room/#match=${inviteCapability}&extra=1`,
  `${origin}/room/#match=${"%41".repeat(43)}`,
  `${origin.replace("https://", "https://user:pass@")}/room/#match=${inviteCapability}`,
  "x".repeat(513)]) {
  assert.equal(boundary.parseInvite(invalid, origin), null, invalid);
}
assert.ok(Object.isFrozen(boundary.parseInvite("123456", origin)));

const created = boundary.ingest(createWire(), null, null, compatibility, origin, now);
assert.ok(created);
assert.equal(created.obsolete, false);
assert.equal(created.state.roomId, roomName);
assert.equal(created.state.endpointId, "100");
assert.equal(created.state.credential, credential);
assert.deepEqual(created.invite, {inviteUrl:
  `${origin}/room/#match=${"A".repeat(43)}`, fallbackCode: "123456",
  inviteGeneration: 1, expiresAt: now + 570_000});
assert.equal(boundary.validate(created.state, compatibility, "100"), true);
assert.ok(Object.isFrozen(created) && Object.isFrozen(created.state) &&
  Object.isFrozen(created.state.lobby) &&
  Object.isFrozen(created.state.lobby.compatibility.buildId) &&
  Object.isFrozen(created.state.lobby.members) &&
  Object.isFrozen(created.state.lobby.members[0]) &&
  Object.isFrozen(created.state.lobby.seats[0]) &&
  Object.isFrozen(created.state.controlTail) && Object.isFrozen(created.invite));

const publication = snapshot();
const retained = boundary.ingest(publication, created.state, created.invite,
  compatibility, origin, now);
assert.ok(retained);
assert.equal(retained.state.roomId, created.state.roomId);
assert.equal(retained.state.credential, created.state.credential);
assert.equal(retained.invite, created.invite,
  "an unchanged generation retains the already-held invitation");
const malformedCustody = boundary.ingest(publication, created.state,
  {...created.invite, extra: true}, compatibility, origin, now);
assert.ok(malformedCustody && malformedCustody.invite === null,
  "malformed prior invite custody fails closed without rejecting public state");
const skewed = boundary.ingest({...createWire(), inviteExpiresAt: now}, null,
  null, compatibility, origin, now);
assert.ok(skewed && skewed.invite,
  "client clock skew cannot invalidate a receipt-relative invitation");
rejected(createWire(), null, null, compatibility, origin,
  Number.MAX_SAFE_INTEGER);
const expiredCustody = boundary.ingest(publication,
  created.state, created.invite, compatibility, origin, created.invite.expiresAt);
assert.ok(expiredCustody && expiredCustody.invite === null,
  "an expired deadline destroys a retained invitation secret");
rejected(createWire(), created.state, created.invite, compatibility, origin,
  created.invite.expiresAt);
const leadershipLost = boundary.ingest(changed(publication, (value) => {
  value.lobby.revision = 2;
  value.lobby.leaderGeneration = 2;
  value.lobby.members.push(member("200"));
  value.lobby.seats.push(seat("200"));
  value.lobby.leaderEndpointId = "200";
}), created.state, created.invite, compatibility, origin, now);
assert.ok(leadershipLost && leadershipLost.invite === null,
  "leadership loss destroys launcher invitation custody");

const revoked = boundary.ingest({...snapshot(), inviteGeneration: 2},
  created.state, created.invite, compatibility, origin, now);
assert.ok(revoked);
assert.equal(revoked.invite, null,
  "a generation change without a replacement secret revokes the stale invitation");
rejected({...snapshot(), inviteExpiresAt: snapshot().inviteExpiresAt - 1},
  created.state, created.invite);
rejected({...snapshot(), expiresAt: snapshot().expiresAt + 1},
  created.state, created.invite);
const rotated = boundary.ingest({...snapshot(), inviteGeneration: 2,
  fallbackCode: "654321", inviteExpiresInMs: 300_000,
  inviteUrl: `${origin}/room/#match=${"B".repeat(43)}`},
created.state, created.invite, compatibility, origin, now, 2);
assert.ok(rotated);
assert.equal(rotated.invite.inviteGeneration, 2);
assert.equal(rotated.invite.fallbackCode, "654321");

const joined = boundary.ingest({...snapshot({lobby: {
  members: [member("100"), member("200")],
  seats: [seat("100"), seat("200")], revision: 2}}),
roomId: roomName, endpointId: "200", credential: "G".repeat(43)},
null, null, compatibility, origin, now);
assert.ok(joined && joined.invite === null);

for (const mutate of [
  (value) => { value.extra = true; },
  (value) => { delete value.schemaVersion; },
  (value) => { value.type = "room_state"; },
  (value) => { value.roomId = "../not-a-room"; },
  (value) => { value.endpointId = "0"; },
  (value) => { value.credential = "short"; },
  (value) => { value.expiresAt = 0; },
  (value) => { value.inviteExpiresAt = value.expiresAt + 1; },
  (value) => { value.inviteGeneration = 0; },
  (value) => { value.fallbackCode = "123 456"; },
  (value) => { value.inviteExpiresInMs = 600_001; },
  (value) => { value.inviteUrl = `https://evil.example/room/#match=${"A".repeat(43)}`; },
  (value) => { value.inviteUrl = `${origin}/room/#match=${"A".repeat(43)}&extra=1`; },
  (value) => { value.lobby = null; },
  (value) => { value.lobby.extra = true; },
  (value) => { value.lobby.compatibility.extra = true; },
  (value) => { value.lobby.members[0].lastCommandId = "1"; },
  (value) => { value.lobby.seats[0].extra = true; },
  (value) => { value.lobby.receipts = []; },
  (value) => { value.lobby.nextReceipt = 0; },
]) rejected(changed(createWire(), mutate));

rejected(createWire({endpointId: "200"}), created.state, created.invite);
rejected(createWire({credential: "D".repeat(43)}), created.state, created.invite);
rejected(createWire({roomId: "zyxwvutsrqponmlkjihgfe"}),
  created.state, created.invite);

for (const endpointId of ["00", "01", "18446744073709551616", "-1", "1e2"]) {
  rejected(changed(createWire(), (value) => { value.lobby.roomId = endpointId; }));
}
const u64Edge = changed(createWire(), (value) => {
  value.lobby.roomId = "18446744073709551615";
});
assert.ok(boundary.ingest(u64Edge, null, null, compatibility, origin, now));

for (const mutate of [
  (value) => { value.lobby.members.push(member("100")); },
  (value) => { value.lobby.members[0].seatCount = 2; },
  (value) => { value.lobby.seats.push(seat("100")); },
  (value) => { value.lobby.seats[0].localIndex = 1; },
  (value) => { value.lobby.seats[0].characterId = 10; },
  (value) => { value.lobby.seats[0].vehicleId = 3; },
  (value) => { value.lobby.seats[0].voteTrack = 256; },
  (value) => { value.lobby.seats[0].characterId = 1; },
  (value) => { value.lobby.members[0].ready = true; },
  (value) => { value.lobby.leaderEndpointId = "200"; },
]) rejected(changed(createWire(), mutate));

const selected = changed(createWire(), (value) => {
  value.lobby.seats[0].characterId = 1;
  value.lobby.seats[0].vehicleId = 0;
  value.lobby.seats[0].selectionRevision = 2;
  value.lobby.members[0].ready = true;
});
assert.ok(boundary.ingest(selected, null, null, compatibility, origin, now));

const step = {accepted: true, duplicate: false, leaderChanged: false,
  error: "ok", revision: 2, matchEpoch: 0, leaderEndpointId: "100",
  selectedTrack: null, selectedVehicleMask: 0};
const withTail = changed(createWire(), (value) => {
  value.lobby.revision = 2;
  value.controlTail = [step];
});
assert.ok(boundary.ingest(withTail, null, null, compatibility, origin, now));
const advanced = boundary.ingest(withTail, created.state, created.invite,
  compatibility, origin, now);
assert.ok(advanced);
const obsolete = boundary.ingest(snapshot(), advanced.state, advanced.invite,
  compatibility, origin, now);
assert.deepEqual(obsolete, {obsolete: true, state: null, invite: null});
const rotatedPublic = changed(withTail, (value) => {
  value.inviteGeneration = 2;
  value.inviteExpiresAt++;
  delete value.fallbackCode;
  delete value.inviteExpiresInMs;
  delete value.inviteUrl;
});
const advancedWithoutSecret = boundary.ingest(rotatedPublic, created.state, null,
  compatibility, origin, now);
const delayedRotateWire = changed(createWire(), (value) => {
  value.inviteGeneration = 2;
  value.inviteExpiresAt++;
  value.fallbackCode = "654321";
  value.inviteUrl = `${origin}/room/#match=${"B".repeat(43)}`;
});
assert.equal(boundary.ingest(delayedRotateWire,
  advancedWithoutSecret.state, null, compatibility, origin, now), null,
"an uncorrelated same-generation response cannot restore a secret");
const delayedRotateSecret = boundary.ingest(delayedRotateWire,
  advancedWithoutSecret.state, null, compatibility, origin, now, 2);
assert.equal(delayedRotateSecret.obsolete, true);
assert.equal(delayedRotateSecret.state, null);
// Tied publication: not newer, so it installs nothing. Nothing was held here,
// so custody stays empty rather than picking up the delayed response's secret.
assert.equal(delayedRotateSecret.invite, null,
  "a delayed response that ties on every ordering pair installs no secret");
// The refusal above is about differing content under tied counters, not about
// delay itself: re-delivering the identical held secret stays idempotent.
const tiedIdenticalRedelivery = boundary.ingest(createWire(),
  created.state, created.invite, compatibility, origin, now, 1);
assert.equal(tiedIdenticalRedelivery.obsolete, false);
assert.notEqual(tiedIdenticalRedelivery.state, null);
assert.equal(tiedIdenticalRedelivery.invite, created.invite,
  "a tied re-delivery of the identical secret keeps held custody unchanged");
const oldGenerationSecret = boundary.ingest(createWire(),
  advancedWithoutSecret.state, null, compatibility, origin, now, 2);
assert.deepEqual(oldGenerationSecret,
  {obsolete: true, state: null, invite: null},
  "an obsolete generation can never revive invitation custody");
rejected(changed(createWire(), (value) => {
  value.fallbackCode = "12345X";
}), advanced.state, advanced.invite);
rejected(changed(snapshot(), (value) => {
  value.inviteExpiresAt--;
}), advanced.state, advanced.invite);
rejected(changed(withTail, (value) => {
  value.lobby.members[0].connected = false;
}), advanced.state, advanced.invite);
rejected(changed(snapshot(), (value) => {
  value.inviteGeneration = 2;
  value.inviteExpiresAt++;
}), advanced.state, advanced.invite);
for (const mutate of [
  (value) => { value.controlTail[0].accepted = false; },
  (value) => { value.controlTail[0].duplicate = true; },
  (value) => { value.controlTail[0].error = "stale_revision"; },
  (value) => { value.controlTail[0].revision = 1; },
  (value) => { value.controlTail[0].extra = true; },
]) rejected(changed(withTail, mutate));
const contiguous = changed(createWire(), (value) => {
  value.lobby.revision = 3;
  value.controlTail = [{...step}, {...step, revision: 3}];
});
assert.ok(boundary.ingest(contiguous, null, null, compatibility, origin, now));
rejected(changed(contiguous, (value) => { value.controlTail[1].revision = 4; }));
rejected(changed(contiguous, (value) => {
  value.controlTail[1].leaderEndpointId = "200";
}));
rejected(changed(contiguous, (value) => {
  value.controlTail[1].selectedTrack = 5;
}));
rejected(changed(contiguous, (value) => {
  value.controlTail[0].matchEpoch = 1;
  value.controlTail[1].matchEpoch = 0;
  value.lobby.matchEpoch = 1;
}));
rejected(changed(contiguous, (value) => {
  value.controlTail = Array.from({length: 65}, (_, index) =>
    ({...step, revision: index + 1}));
  value.lobby.revision = 65;
}));

const activeWire = createWire();
activeWire.lobby = {protocolVersion: 1, revision: 12, matchEpoch: 1,
  leaderGeneration: 2, roomId: "42", leaderEndpointId: "300", phase: "racing",
  compatibility: structuredClone(compatibility),
  members: [member("100", {ready: true}), member("200", {loaded: true}),
    member("300", {ready: true}), member("400", {loaded: true})],
  seats: [seat("100", {selectionRevision: 1, voteTrack: 5,
    characterId: 0, vehicleId: 0}),
  seat("200", {selectionRevision: 1, characterId: 1, vehicleId: 1}),
  seat("300", {selectionRevision: 1, voteTrack: 29,
    characterId: 2, vehicleId: 2}),
  seat("400", {selectionRevision: 1, characterId: 3, vehicleId: 0})],
  selectedTrack: 29, selectedVehicleMask: 7};
activeWire.endpointId = "200";
activeWire.credential = "G".repeat(43);
delete activeWire.fallbackCode;
delete activeWire.inviteExpiresInMs;
delete activeWire.inviteUrl;
const active = boundary.ingest(activeWire, null, null, compatibility, origin, now);
assert.ok(active);
assert.deepEqual(boundary.projection(active.state, compatibility, "200", 6, 2, 0),
  [6, 3, 12, 1, 2, 1, 4, 4, 85, 5, 15, 10, 228, 15, 15, 5,
    29, 7, 2, 0, 0]);
assert.equal(boundary.projection(active.state, compatibility, "200", 6, 2, 1),
  null, "a non-leader cannot project an invitation control");
assert.deepEqual(boundary.projection(
  created.state, compatibility, "100", 1, 1, 1).slice(-1), [1]);
assert.deepEqual(boundary.projection(
  created.state, compatibility, "100", 1, 1, 2).slice(-1), [2]);
assert.equal(boundary.projection(active.state, compatibility, "999", 6, 2, 1),
  null);
assert.equal(boundary.projection(active.state, compatibility, "200", 0, 2, 1),
  null);
assert.equal(boundary.projection(active.state, compatibility, "200", 7, 2, 0),
  null, "room and service phases cannot contradict each other");
assert.ok(Object.isFrozen(boundary.projection(
  active.state, compatibility, "200", 6, 2, 0)));
assert.equal(boundary.projection(created.state, compatibility, "100", 1, 1, true),
  null, "invite state is a bounded enum, not JavaScript truthiness");

for (const mutate of [
  (value) => { value.lobby.selectedTrack = null; },
  (value) => { value.lobby.selectedVehicleMask = 0; },
  (value) => { value.lobby.matchEpoch = 0; },
  (value) => { value.lobby.seats[0].characterId = null; },
  (value) => { value.lobby.selectedVehicleMask = 6; },
]) rejected(changed(activeWire, mutate));

const closed = changed(createWire(), (value) => {
  value.lobby.phase = "closed";
  value.closedReason = "host_closed";
  delete value.fallbackCode;
  delete value.inviteExpiresInMs;
  delete value.inviteUrl;
});
assert.ok(boundary.ingest(closed, null, null, compatibility, origin, now));
const closedState = boundary.ingest(closed, null, null, compatibility, origin, now);
assert.equal(boundary.projection(
  closedState.state, compatibility, "100", 8, 1, 0)[19], 14,
"host-closed service state maps to the bounded recovery reason");
const closedAfterOpen = boundary.ingest(changed(closed, (value) => {
  value.lobby.revision = 2;
  value.inviteExpiresAt = now;
}), created.state, created.invite, compatibility, origin, now);
assert.ok(closedAfterOpen && closedAfterOpen.state.closedReason === "host_closed" &&
  closedAfterOpen.invite === null,
"room closure may atomically revoke the current generation deadline");
const roomExpired = boundary.ingest(changed(closed, (value) => {
  value.closedReason = "room_expired";
}), null, null, compatibility, origin, now);
assert.equal(boundary.projection(
  roomExpired.state, compatibility, "100", 8, 1, 0)[19], 15,
"room-expired service state maps to the bounded recovery reason");
rejected(changed(closed, (value) => { value.closedReason = null; }));
rejected(changed(createWire(), (value) => { value.closedReason = "host_closed"; }));

console.log("online room live-state tests passed");
