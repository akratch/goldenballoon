import assert from "node:assert/strict";
import {webcrypto} from "node:crypto";
import {createMatchPeerIdentity, createMatchPeerReplayWindow,
  createMatchPeerSealWindow, deriveMatchPeerKey,
  deriveMatchPeerKeyFromIdentity, digestMatchPeerTranscript,
  forgetMatchPeerIdentity, forgetMatchPeerKey, inspectMatchPeerEnvelope,
  MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT, matchPeerVerificationPhrase,
  openMatchPeerEnvelope, sealMatchPeerEnvelope}
  from "../dist/web/online/match-peer-crypto.js";
import {createMatchPreflightFragmentState, encodeMatchPreflightFragments,
  MATCH_PREFLIGHT_ALL_FLAGS, MATCH_PREFLIGHT_FRAGMENT_COUNT,
  submitMatchPreflightFragment} from "../dist/web/online/match-preflight.js";

const toHex = value => [...value].map(byte => byte.toString(16).padStart(2, "0")).join("");
const directionOf = value => ({matchEpoch: value.matchEpoch,
  sourceEndpointId: value.sourceEndpointId,
  sourceGeneration: value.sourceGeneration,
  destinationEndpointId: value.destinationEndpointId,
  destinationGeneration: value.destinationGeneration});
async function sealForgedEnvelope(key, forged, sequence = 1n) {
  const aad = new Uint8Array(52);
  aad.set([0x4d, 0x50, 0x45, 0x31, 1,
    forged.intermediateEndpointId === 0n ? 0 : 1, forged.payloadType, 0]);
  const view = new DataView(aad.buffer);
  view.setUint32(8, forged.matchEpoch, false);
  view.setBigUint64(12, forged.sourceEndpointId, false);
  view.setUint32(20, forged.sourceGeneration, false);
  view.setBigUint64(24, forged.destinationEndpointId, false);
  view.setUint32(32, forged.destinationGeneration, false);
  view.setBigUint64(36, forged.intermediateEndpointId, false);
  view.setBigUint64(44, sequence, false);
  const nonce = new Uint8Array(12);
  const nonceView = new DataView(nonce.buffer);
  nonceView.setUint32(0, forged.matchEpoch, false);
  nonceView.setBigUint64(4, sequence, false);
  const encrypted = new Uint8Array(await webcrypto.subtle.encrypt({name: "AES-GCM",
    iv: nonce, additionalData: aad, tagLength: 128}, key.handle, payload));
  const envelope = new Uint8Array(132);
  envelope.set(aad);
  envelope.set(encrypted, 52);
  return envelope;
}
const envelopeHex = "4d50453101010000000000070000000000000064000000020000000000000190" +
  "0000000900000000000000c800000000000000012bd9f1554abd62235a212387" +
  "c3d2cd82030455aa45e53f72d45d982c44ced93ea300c68543d2e9b9c16ce23" +
  "ed3b5d35f57d13a9dbf3a3010fa37df24c6aefaedeb1d33859f2376cde014319" +
  "33d5f1fad";
const secret = Uint8Array.from({length: 32}, (_, index) => index);
const transcript = Uint8Array.from({length: 32}, (_, index) => 0xa0 + index);
const payload = Uint8Array.from({length: 64}, (_, index) => 0x40 + index);
const context = {matchEpoch: 7, sourceEndpointId: 100n, sourceGeneration: 2,
  destinationEndpointId: 400n, destinationGeneration: 9,
  intermediateEndpointId: 200n, payloadType: 0};
const expectedDirection = directionOf(context);
const compatibility = {protocolVersion: 1,
  buildId: Uint8Array.from({length: 16}, (_, index) => index + 1),
  gameplayDigest: Uint8Array.from({length: 32}, (_, index) => 0x80 + index),
  romRevision: 1, cadenceHz: 30};
const entries = [300n, 100n, 200n].map((endpointId, index) => ({endpointId,
  generation: Number(endpointId / 100n),
  publicKey: Uint8Array.from({length: 65}, (_, byte) => byte === 0 ? 4 :
    Number(endpointId / 100n) + byte)}));
const digest = await digestMatchPeerTranscript({roomId: Uint8Array.from(
  {length: 16}, (_, index) => index + 1), matchEpoch: 7, compatibility, entries},
webcrypto);
assert.equal(toHex(digest),
  "83591b0d6f3d7852b564dbe1976a50b9dcb1bdd82114c937f12a903505ec7019");
assert.equal(matchPeerVerificationPhrase(digest),
  "Nimble-Pilot Jolly-Star Sunny-Falcon");
const reorderedDigest = await digestMatchPeerTranscript({roomId: Uint8Array.from(
  {length: 16}, (_, index) => index + 1), matchEpoch: 7, compatibility,
entries: [entries[2], entries[1], entries[0]]}, webcrypto);
assert.equal(toHex(reorderedDigest), toHex(digest));
const leftIdentity = await createMatchPeerIdentity(webcrypto);
const rightIdentity = await createMatchPeerIdentity(webcrypto);
const leftKey = await deriveMatchPeerKeyFromIdentity(leftIdentity,
  rightIdentity.publicKey, digest, context, webcrypto);
const rightKey = await deriveMatchPeerKeyFromIdentity(rightIdentity,
  leftIdentity.publicKey, digest, context, webcrypto);
const leftIdentityWindow = createMatchPeerSealWindow(leftKey, expectedDirection);
const rightIdentityWindow = createMatchPeerSealWindow(rightKey, expectedDirection);
const leftIdentityEnvelope = await sealMatchPeerEnvelope(
  leftKey, leftIdentityWindow, context, payload, webcrypto);
const rightIdentityEnvelope = await sealMatchPeerEnvelope(
  rightKey, rightIdentityWindow, context, payload, webcrypto);
assert.equal(toHex(leftIdentityEnvelope), toHex(rightIdentityEnvelope));
forgetMatchPeerIdentity(leftIdentity);
await assert.rejects(() => deriveMatchPeerKeyFromIdentity(leftIdentity,
  rightIdentity.publicKey, digest, context, webcrypto), /invalid match peer identity/);
forgetMatchPeerIdentity(rightIdentity);
const key = await deriveMatchPeerKey(secret, transcript, context, webcrypto);
const directionCheckKey = await deriveMatchPeerKey(
  secret, transcript, context, webcrypto);
assert.throws(() => createMatchPeerSealWindow(directionCheckKey,
  {...expectedDirection, sourceGeneration: 3}),
  /invalid match peer seal direction/);
const directionCheckWindow = createMatchPeerSealWindow(
  directionCheckKey, expectedDirection);
assert.equal(directionCheckWindow.nextSequence, 1n);
forgetMatchPeerKey(directionCheckKey);
const sealWindow = createMatchPeerSealWindow(key, expectedDirection);
assert.throws(() => createMatchPeerSealWindow(key, expectedDirection),
  /invalid match peer seal direction/);
assert.equal(key.handle.extractable, false);
await assert.rejects(() => sealMatchPeerEnvelope(
  key, sealWindow, {...context, payloadType: 2}, payload, webcrypto),
  /invalid match peer envelope/);
assert.equal(sealWindow.nextSequence, 1n);
await assert.rejects(() => sealMatchPeerEnvelope(
  key, sealWindow, {...context, sequence: 1n}, payload, webcrypto),
  /invalid match peer envelope/);
assert.equal(sealWindow.nextSequence, 1n);
const envelope = await sealMatchPeerEnvelope(
  key, sealWindow, context, payload, webcrypto);
assert.equal(toHex(envelope), envelopeHex);
assert.deepEqual(inspectMatchPeerEnvelope(envelope), {...context, sequence: 1n});
assert.equal(sealWindow.nextSequence, 2n);
assert.throws(() => { sealWindow.nextSequence = 1n; }, TypeError);

const replay = createMatchPeerReplayWindow();
const opened = await openMatchPeerEnvelope(key, expectedDirection, replay,
  envelope, webcrypto);
assert.equal(opened.result, "ok");
assert.deepEqual(opened.payload, payload);
assert.equal(replay.greatestSequence, 1n);
const duplicate = await openMatchPeerEnvelope(key, expectedDirection, replay,
  envelope, webcrypto);
assert.equal(duplicate.result, "replay");

// The real fixed 124-byte attestation crosses the 64-byte carrier as three
// independently authenticated type-1 payloads. Transport sequence is global
// for the direction and independent of report sequence and fragment order.
const report = {matchEpoch: 7, connectionGeneration: 2, sequence: 13,
  endpointId: 100n, flags: MATCH_PREFLIGHT_ALL_FLAGS,
  descriptorDigest: Uint8Array.from({length: 32}, (_, index) => index + 1),
  transcriptDigest: Uint8Array.from({length: 32}, (_, index) => index + 0x41),
  graphDigest: Uint8Array.from({length: 32}, (_, index) => index + 0x81)};
const fragmentPayloads = encodeMatchPreflightFragments(report);
assert.equal(fragmentPayloads.length, MATCH_PREFLIGHT_FRAGMENT_COUNT);
const fragmentState = createMatchPreflightFragmentState(context);
const fragmentOrder = [2, 0, 1];
for (let index = 0; index < fragmentOrder.length; index++) {
  const preflightContext = {...context,
    payloadType: MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT};
  const preflightEnvelope = await sealMatchPeerEnvelope(key, sealWindow,
    preflightContext, fragmentPayloads[fragmentOrder[index]], webcrypto);
  assert.equal(preflightEnvelope[6], MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
  const preflightOpened = await openMatchPeerEnvelope(key, expectedDirection, replay,
    preflightEnvelope, webcrypto);
  assert.equal(preflightOpened.result, "ok");
  assert.equal(preflightOpened.context.payloadType,
    MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
  assert.equal(preflightOpened.context.sequence, BigInt(2 + index));
  const submitted = submitMatchPreflightFragment(fragmentState,
    preflightOpened.context, preflightOpened.payload);
  assert.equal(submitted.result,
    index + 1 === fragmentOrder.length ? "complete" : "accepted");
  if (submitted.result === "complete") assert.deepEqual(submitted.attestation, report);
}

// Sealing is monotonic even when delivery is reordered. The receiver accepts
// unseen packets within 63 prior sequence positions and rejects older ones.
const envelope5 = await sealMatchPeerEnvelope(
  key, sealWindow, context, payload, webcrypto);
const envelope6 = await sealMatchPeerEnvelope(
  key, sealWindow, context, payload, webcrypto);
assert.equal((await openMatchPeerEnvelope(key, expectedDirection, replay,
  envelope6, webcrypto)).context.sequence, 6n);
assert.equal((await openMatchPeerEnvelope(key, expectedDirection, replay,
  envelope5, webcrypto)).context.sequence, 5n);
const envelope7 = await sealMatchPeerEnvelope(
  key, sealWindow, context, payload, webcrypto);
let envelope71;
for (let sequence = 8n; sequence <= 71n; sequence++) {
  envelope71 = await sealMatchPeerEnvelope(
    key, sealWindow, context, payload, webcrypto);
  assert.equal(inspectMatchPeerEnvelope(envelope71).sequence, sequence);
}
assert.equal((await openMatchPeerEnvelope(key, expectedDirection, replay,
  envelope71, webcrypto)).result, "ok");
assert.equal((await openMatchPeerEnvelope(key, expectedDirection, replay,
  envelope7, webcrypto)).result, "replay");

for (const corrupt of [
  {initialized: true, greatestSequence: 0n, seenBitmap: 0n},
  {initialized: false, greatestSequence: 1n, seenBitmap: 1n},
  {initialized: true, greatestSequence: 1n, seenBitmap: 0n},
]) {
  const before = {...corrupt};
  const result = await openMatchPeerEnvelope(key, expectedDirection, corrupt,
    envelope, webcrypto);
  assert.equal(result.result, "invalid");
  assert.deepEqual(corrupt, before);
}

for (let index = 0; index < envelope.length; index++) {
  const mutated = envelope.slice();
  mutated[index] ^= 0x40;
  const fresh = createMatchPeerReplayWindow();
  const result = await openMatchPeerEnvelope(key, expectedDirection, fresh,
    mutated, webcrypto);
  assert.notEqual(result.result, "ok", `mutation ${index}`);
  assert.equal(fresh.initialized, false);
}

const reverse = {...context, sourceEndpointId: 400n, sourceGeneration: 9,
  destinationEndpointId: 100n, destinationGeneration: 2};
const reverseDirection = directionOf(reverse);
const reverseKey = await deriveMatchPeerKey(secret, transcript, reverse, webcrypto);
const reverseWindow = createMatchPeerSealWindow(reverseKey, reverseDirection);
const reverseEnvelope = await sealMatchPeerEnvelope(
  reverseKey, reverseWindow, reverse, payload, webcrypto);
assert.notEqual(toHex(reverseEnvelope), envelopeHex);
assert.equal((await openMatchPeerEnvelope(key, {...expectedDirection, matchEpoch: 6},
  createMatchPeerReplayWindow(), envelope, webcrypto)).result, "stale_epoch");
assert.equal((await openMatchPeerEnvelope(key, {...expectedDirection,
  destinationEndpointId: 300n},
  createMatchPeerReplayWindow(), envelope, webcrypto)).result, "wrong_recipient");
assert.equal((await openMatchPeerEnvelope(key, {...expectedDirection,
  sourceEndpointId: 0n},
  createMatchPeerReplayWindow(), envelope, webcrypto)).result, "invalid");

// Key selection and claimed header identity are separate inputs. Even if a
// direct-channel caller supplies this peer's valid key, the protected header
// cannot claim a different source endpoint or generation.
const forgedSource = {...context, sourceEndpointId: 300n, sourceGeneration: 5};
const forgedSourceEnvelope = await sealForgedEnvelope(key, forgedSource);
assert.equal((await openMatchPeerEnvelope(key, expectedDirection,
  createMatchPeerReplayWindow(), forgedSourceEnvelope, webcrypto)).result,
  "wrong_source");
const forgedGeneration = {...context, sourceGeneration: 3};
const forgedGenerationEnvelope = await sealForgedEnvelope(key, forgedGeneration);
assert.equal((await openMatchPeerEnvelope(key, expectedDirection,
  createMatchPeerReplayWindow(), forgedGenerationEnvelope, webcrypto)).result,
  "stale_generation");

forgetMatchPeerKey(reverseKey);
await assert.rejects(() => sealMatchPeerEnvelope(
  reverseKey, reverseWindow, reverse, payload, webcrypto),
  /invalid match peer envelope/);

// WebCrypto is asynchronous: a second send must not observe the same sequence
// while the first encryption is awaiting the provider. Provider failure clears
// the busy latch without consuming the nonce.
const concurrentContext = {...context, destinationGeneration: 10};
const concurrentDirection = directionOf(concurrentContext);
const concurrentKey = await deriveMatchPeerKey(
  secret, transcript, concurrentDirection, webcrypto);
const concurrentWindow = createMatchPeerSealWindow(
  concurrentKey, concurrentDirection);
let releaseEncrypt;
let reportEncryptEntered;
const encryptEntered = new Promise(resolve => { reportEncryptEntered = resolve; });
const encryptRelease = new Promise(resolve => { releaseEncrypt = resolve; });
const blockingProvider = {subtle: {encrypt: async (...args) => {
  reportEncryptEntered();
  await encryptRelease;
  return webcrypto.subtle.encrypt(...args);
}}};
const inFlight = sealMatchPeerEnvelope(concurrentKey, concurrentWindow,
  concurrentContext, payload, blockingProvider);
await encryptEntered;
await assert.rejects(() => sealMatchPeerEnvelope(concurrentKey, concurrentWindow,
  concurrentContext, payload, webcrypto), /invalid match peer envelope/);
assert.equal(concurrentWindow.nextSequence, 1n);
releaseEncrypt();
assert.equal(inspectMatchPeerEnvelope(await inFlight).sequence, 1n);
assert.equal(concurrentWindow.nextSequence, 2n);
const failingProvider = {subtle: {encrypt: async () => {
  throw new Error("injected provider failure");
}}};
await assert.rejects(() => sealMatchPeerEnvelope(concurrentKey, concurrentWindow,
  concurrentContext, payload, failingProvider), /injected provider failure/);
assert.equal(concurrentWindow.nextSequence, 2n);
assert.equal(inspectMatchPeerEnvelope(await sealMatchPeerEnvelope(
  concurrentKey, concurrentWindow, concurrentContext, payload, webcrypto)).sequence, 2n);
forgetMatchPeerKey(concurrentKey);

console.log("test_match_peer_crypto_js: PASS");
