// Recipient-encrypted match payload envelope. WebCrypto is the browser half of
// platform/net/match_peer_crypto.cpp; the checked vector locks both together.
export const MATCH_PEER_PAYLOAD_BYTES = 64;
export const MATCH_PEER_HEADER_BYTES = 52;
export const MATCH_PEER_ENVELOPE_BYTES = 132;
export const MATCH_PEER_PAYLOAD_INPUT = 0;
export const MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT = 1;

const MAGIC = new Uint8Array([0x4d, 0x50, 0x45, 0x31]);
const KEY_DOMAIN = new TextEncoder().encode("golden-balloon-match-input-key-v2");
// v2 is the committed-transcript protocol: round 1 publishes a hiding
// commitment to each endpoint's public key, round 2 opens it. A v1 peer derives
// a different digest and therefore a different key, so the versions cannot
// interoperate or negotiate down to the grindable phrase.
const TRANSCRIPT_DOMAIN = new TextEncoder().encode(
  "golden-balloon-match-peer-transcript-v2");
const COMMIT_DOMAIN = new TextEncoder().encode(
  "golden-balloon-match-peer-commit-v1");
// Envelope protocol version. v1 predates the key-commitment round; a v1
// envelope is rejected before decryption rather than negotiated down.
export const MATCH_PEER_VERSION = 2;
export const MATCH_PEER_COMMIT_NONCE_BYTES = 32;
export const MATCH_PEER_COMMIT_BYTES = 32;
const LEFT = ["Amber", "Brave", "Bright", "Calm", "Coral", "Cosmic", "Daring",
  "Flying", "Gentle", "Golden", "Happy", "Icy", "Jolly", "Lucky", "Mighty",
  "Neon", "Nimble", "Orange", "Rapid", "Royal", "Silver", "Solar", "Sunny",
  "Swift", "Teal", "Tiny", "Turbo", "Velvet", "Violet", "Warm", "Wild", "Zippy"];
const RIGHT = ["Balloon", "Comet", "Drum", "Falcon", "Fox", "Glider", "Kite",
  "Lion", "Moon", "Otter", "Panda", "Parrot", "Pebble", "Pilot", "Rocket",
  "Sail", "Sparrow", "Star", "Tiger", "Toucan", "Turtle", "Whale", "Wing",
  "Cloud", "Dolphin", "Lantern", "Meteor", "Penguin", "Planet", "Raven",
  "Sunrise", "Thunder"];
const U32_MAX = 0xffff_ffff;
const U64_MAX = (1n << 64n) - 1n;
const SEND_CONTEXT_FIELDS = new Set(["matchEpoch", "sourceEndpointId",
  "sourceGeneration", "destinationEndpointId", "destinationGeneration",
  "intermediateEndpointId", "payloadType"]);
const SEAL_WINDOWS = new WeakMap();
// Belt and braces under the registry below: even reached directly, a key can
// only ever be given one window.
const ISSUED_SEAL_KEYS = new WeakSet();
// Derived keys, named by a hash of the inputs that produce them. Deriving the
// identical (secret, transcript, direction) twice returns the SAME record, so
// the sequence space cannot restart under one key — two windows over one AES
// key would repeat a nonce and leak the GHASH subkey. Keyed on the inputs
// rather than the key material, so nothing key-derived is retained.
const KEY_REGISTRY = new Map();
const KEY_FINGERPRINT_DOMAIN = new TextEncoder().encode(
  "golden-balloon-match-peer-key-fingerprint-v1");

function hex(value) {
  return [...value].map(byte => byte.toString(16).padStart(2, "0")).join("");
}

async function keyFingerprint(sharedSecret, transcriptDigest, info, provider) {
  const output = new Uint8Array(KEY_FINGERPRINT_DOMAIN.length +
    sharedSecret.byteLength + transcriptDigest.byteLength + info.byteLength);
  let offset = 0;
  output.set(KEY_FINGERPRINT_DOMAIN, offset);
  offset += KEY_FINGERPRINT_DOMAIN.length;
  output.set(sharedSecret, offset); offset += sharedSecret.byteLength;
  output.set(transcriptDigest, offset); offset += transcriptDigest.byteLength;
  output.set(info, offset);
  const digest = new Uint8Array(await provider.subtle.digest("SHA-256", output));
  output.fill(0);
  return hex(digest);
}

function bytes(value, length) {
  return value instanceof Uint8Array && value.byteLength === length;
}

function u32(value, nonzero = false) {
  return Number.isInteger(value) && value >= (nonzero ? 1 : 0) && value <= U32_MAX;
}

function u64(value, nonzero = false) {
  return typeof value === "bigint" && value >= (nonzero ? 1n : 0n) && value <= U64_MAX;
}

function keyContextValid(value) {
  return value && u32(value.matchEpoch, true) && u64(value.sourceEndpointId, true) &&
    u32(value.sourceGeneration, true) && u64(value.destinationEndpointId, true) &&
    u32(value.destinationGeneration, true) &&
    value.sourceEndpointId !== value.destinationEndpointId;
}

function envelopeContextValid(value) {
  return value && keyContextValid(value) && u64(value.intermediateEndpointId) &&
    u64(value.sequence, true) && Number.isInteger(value.payloadType) &&
    value.payloadType >= MATCH_PEER_PAYLOAD_INPUT &&
    value.payloadType <= MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT &&
    (value.intermediateEndpointId === 0n ||
      (value.intermediateEndpointId !== value.sourceEndpointId &&
       value.intermediateEndpointId !== value.destinationEndpointId));
}

function sendContextValid(value) {
  return value && typeof value === "object" &&
    Object.keys(value).length === SEND_CONTEXT_FIELDS.size &&
    Object.keys(value).every(field => SEND_CONTEXT_FIELDS.has(field)) &&
    keyContextValid(value) && u64(value.intermediateEndpointId) &&
    Number.isInteger(value.payloadType) &&
    value.payloadType >= MATCH_PEER_PAYLOAD_INPUT &&
    value.payloadType <= MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT &&
    (value.intermediateEndpointId === 0n ||
      (value.intermediateEndpointId !== value.sourceEndpointId &&
       value.intermediateEndpointId !== value.destinationEndpointId));
}

function directionEqual(left, right) {
  return left.matchEpoch === right.matchEpoch &&
    left.sourceEndpointId === right.sourceEndpointId &&
    left.sourceGeneration === right.sourceGeneration &&
    left.destinationEndpointId === right.destinationEndpointId &&
    left.destinationGeneration === right.destinationGeneration;
}

function put32(view, offset, value) { view.setUint32(offset, value, false); }
function put64(view, offset, value) { view.setBigUint64(offset, value, false); }

function keyInfo(context) {
  const output = new Uint8Array(KEY_DOMAIN.length + 28);
  output.set(KEY_DOMAIN);
  const view = new DataView(output.buffer);
  let offset = KEY_DOMAIN.length;
  put32(view, offset, context.matchEpoch); offset += 4;
  put64(view, offset, context.sourceEndpointId); offset += 8;
  put32(view, offset, context.sourceGeneration); offset += 4;
  put64(view, offset, context.destinationEndpointId); offset += 8;
  put32(view, offset, context.destinationGeneration);
  return output;
}

// Round 1: published before any public key is revealed, so no endpoint can
// choose its key after seeing another's. The epoch and authenticated identity
// are bound in, so a commitment cannot be lifted into another room or epoch.
export async function matchPeerCommitment(matchEpoch, endpointId, generation,
                                          nonce, publicKey,
                                          provider = globalThis.crypto) {
  if (!provider?.subtle || !u32(matchEpoch, true) || !u64(endpointId, true) ||
      !u32(generation, true) || !bytes(nonce, MATCH_PEER_COMMIT_NONCE_BYTES) ||
      !nonce.some(byte => byte !== 0) || !bytes(publicKey, 65) ||
      publicKey[0] !== 4 || !publicKey.subarray(1).some(byte => byte !== 0))
    throw new TypeError("invalid match peer commitment input");
  const output = new Uint8Array(COMMIT_DOMAIN.length + 16 +
    MATCH_PEER_COMMIT_NONCE_BYTES + 65);
  output.set(COMMIT_DOMAIN);
  const view = new DataView(output.buffer);
  let offset = COMMIT_DOMAIN.length;
  put32(view, offset, matchEpoch); offset += 4;
  put64(view, offset, endpointId); offset += 8;
  put32(view, offset, generation); offset += 4;
  output.set(nonce, offset); offset += MATCH_PEER_COMMIT_NONCE_BYTES;
  output.set(publicKey, offset);
  return new Uint8Array(await provider.subtle.digest("SHA-256", output));
}

// Round 2. Constant-time comparison; false rather than throwing on mismatch.
export async function verifyMatchPeerCommitment(matchEpoch, endpointId,
                                                generation, nonce, publicKey,
                                                commitment,
                                                provider = globalThis.crypto) {
  if (!bytes(commitment, MATCH_PEER_COMMIT_BYTES)) return false;
  let expected;
  try {
    expected = await matchPeerCommitment(matchEpoch, endpointId, generation,
      nonce, publicKey, provider);
  } catch (_) { return false; }
  let difference = 0;
  for (let index = 0; index < MATCH_PEER_COMMIT_BYTES; index++)
    difference |= expected[index] ^ commitment[index];
  return difference === 0;
}

export async function digestMatchPeerTranscript(transcript,
                                                provider = globalThis.crypto) {
  const compatibility = transcript?.compatibility;
  const entries = transcript?.entries;
  if (!provider?.subtle || !bytes(transcript?.roomId, 16) ||
      !transcript.roomId.some(byte => byte !== 0) || !u32(transcript.matchEpoch, true) ||
      !compatibility || compatibility.protocolVersion !== 1 ||
      !bytes(compatibility.buildId, 16) || !compatibility.buildId.some(byte => byte !== 0) ||
      !bytes(compatibility.gameplayDigest, 32) ||
      !compatibility.gameplayDigest.some(byte => byte !== 0) ||
      ![1, 2].includes(compatibility.romRevision) ||
      ![25, 30].includes(compatibility.cadenceHz) || !Array.isArray(entries) ||
      entries.length < 2 || entries.length > 4 || entries.some(entry =>
        !u64(entry?.endpointId, true) || !u32(entry?.generation, true) ||
        !bytes(entry?.publicKey, 65) || entry.publicKey[0] !== 4 ||
        !entry.publicKey.subarray(1).some(byte => byte !== 0) ||
        !bytes(entry?.commitment, MATCH_PEER_COMMIT_BYTES) ||
        !entry.commitment.some(byte => byte !== 0) ||
        !bytes(entry?.commitNonce, MATCH_PEER_COMMIT_NONCE_BYTES) ||
        !entry.commitNonce.some(byte => byte !== 0)) ||
      new Set(entries.map(entry => entry.endpointId)).size !== entries.length)
    throw new TypeError("invalid match peer transcript");
  const sorted = [...entries].sort((left, right) =>
    left.endpointId < right.endpointId ? -1 : left.endpointId > right.endpointId ? 1 : 0);
  // Re-run round 2 here, not only at the call site: a phrase derived from a key
  // that was never committed to is exactly what an active MITM wants, so the
  // digest refuses rather than trusting the caller to have verified.
  for (const entry of sorted) {
    if (!await verifyMatchPeerCommitment(transcript.matchEpoch, entry.endpointId,
        entry.generation, entry.commitNonce, entry.publicKey, entry.commitment,
        provider))
      throw new TypeError("match peer commitment does not open");
  }
  const output = new Uint8Array(TRANSCRIPT_DOMAIN.length + 16 + 4 + 4 + 16 + 32 +
    3 + sorted.length * 109);
  output.set(TRANSCRIPT_DOMAIN);
  let offset = TRANSCRIPT_DOMAIN.length;
  output.set(transcript.roomId, offset); offset += 16;
  const view = new DataView(output.buffer);
  put32(view, offset, compatibility.protocolVersion); offset += 4;
  put32(view, offset, transcript.matchEpoch); offset += 4;
  output.set(compatibility.buildId, offset); offset += 16;
  output.set(compatibility.gameplayDigest, offset); offset += 32;
  output[offset++] = compatibility.romRevision;
  output[offset++] = compatibility.cadenceHz;
  output[offset++] = sorted.length;
  for (const entry of sorted) {
    put64(view, offset, entry.endpointId); offset += 8;
    put32(view, offset, entry.generation); offset += 4;
    // The commitment is hashed as well as the key it opens, so the phrase
    // covers the whole two-round handshake rather than only its outcome.
    output.set(entry.commitment, offset); offset += MATCH_PEER_COMMIT_BYTES;
    output.set(entry.publicKey, offset); offset += 65;
  }
  if (offset !== output.length) throw new Error("match transcript size mismatch");
  return new Uint8Array(await provider.subtle.digest("SHA-256", output));
}

export function matchPeerVerificationPhrase(digest) {
  if (!bytes(digest, 32)) throw new TypeError("invalid match peer digest");
  const value = (digest[0] << 22) | (digest[1] << 14) |
    (digest[2] << 6) | (digest[3] >>> 2);
  return `${LEFT[(value >>> 25) & 31]}-${RIGHT[(value >>> 20) & 31]} ` +
    `${LEFT[(value >>> 15) & 31]}-${RIGHT[(value >>> 10) & 31]} ` +
    `${LEFT[(value >>> 5) & 31]}-${RIGHT[value & 31]}`;
}

function header(context) {
  const output = new Uint8Array(MATCH_PEER_HEADER_BYTES);
  output.set(MAGIC);
  output[4] = MATCH_PEER_VERSION;
  output[5] = context.intermediateEndpointId === 0n ? 0 : 1;
  output[6] = context.payloadType;
  const view = new DataView(output.buffer);
  put32(view, 8, context.matchEpoch);
  put64(view, 12, context.sourceEndpointId);
  put32(view, 20, context.sourceGeneration);
  put64(view, 24, context.destinationEndpointId);
  put32(view, 32, context.destinationGeneration);
  put64(view, 36, context.intermediateEndpointId);
  put64(view, 44, context.sequence);
  return output;
}

function parseHeader(envelope) {
  if (!bytes(envelope, MATCH_PEER_ENVELOPE_BYTES) ||
      MAGIC.some((byte, index) => envelope[index] !== byte) || envelope[4] !== MATCH_PEER_VERSION ||
      envelope[5] > 1 || envelope[6] > MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT ||
      envelope[7] !== 0) return null;
  const view = new DataView(envelope.buffer, envelope.byteOffset, MATCH_PEER_HEADER_BYTES);
  const context = {matchEpoch: view.getUint32(8, false),
    sourceEndpointId: view.getBigUint64(12, false),
    sourceGeneration: view.getUint32(20, false),
    destinationEndpointId: view.getBigUint64(24, false),
    destinationGeneration: view.getUint32(32, false),
    intermediateEndpointId: view.getBigUint64(36, false),
    sequence: view.getBigUint64(44, false), payloadType: envelope[6]};
  return envelopeContextValid(context) &&
    ((envelope[5] === 0) === (context.intermediateEndpointId === 0n)) ? context : null;
}

function nonce(context) {
  const output = new Uint8Array(12);
  const view = new DataView(output.buffer);
  put32(view, 0, context.matchEpoch);
  put64(view, 4, context.sequence);
  return output;
}

function usableKey(key) {
  return key && key.handle instanceof CryptoKey && key.destroyed === false &&
    keyContextValid(key.direction);
}

export async function deriveMatchPeerKey(sharedSecret, transcriptDigest, context,
                                         provider = globalThis.crypto) {
  if (!provider?.subtle || !bytes(sharedSecret, 32) || !bytes(transcriptDigest, 32) ||
      !keyContextValid(context)) throw new TypeError("invalid match peer key context");
  const fingerprint = await keyFingerprint(sharedSecret, transcriptDigest,
    keyInfo(context), provider);
  const existing = KEY_REGISTRY.get(fingerprint);
  // Same inputs, same key: hand back the live window rather than a second one
  // whose sequence would restart at 1 under the same key.
  if (existing && existing.key.destroyed === false) return existing;
  const material = await provider.subtle.importKey("raw", sharedSecret, "HKDF", false,
    ["deriveKey"]);
  const handle = await provider.subtle.deriveKey({name: "HKDF", hash: "SHA-256",
    salt: transcriptDigest, info: keyInfo(context)}, material,
  {name: "AES-GCM", length: 256}, false, ["encrypt", "decrypt"]);
  const direction = Object.freeze({matchEpoch: context.matchEpoch,
    sourceEndpointId: context.sourceEndpointId,
    sourceGeneration: context.sourceGeneration,
    destinationEndpointId: context.destinationEndpointId,
    destinationGeneration: context.destinationGeneration});
  const key = {handle, direction, destroyed: false, fingerprint};
  const record = Object.freeze({key, sealWindow: mintSealWindow(key, direction)});
  KEY_REGISTRY.set(fingerprint, record);
  return record;
}

export async function createMatchPeerIdentity(provider = globalThis.crypto) {
  if (!provider?.subtle) throw new TypeError("WebCrypto is unavailable");
  const keys = await provider.subtle.generateKey(
    {name: "ECDH", namedCurve: "P-256"}, false, ["deriveBits"]);
  const publicKey = new Uint8Array(await provider.subtle.exportKey("raw", keys.publicKey));
  if (!bytes(publicKey, 65) || publicKey[0] !== 4)
    throw new Error("unexpected P-256 public key");
  return {privateKey: keys.privateKey, publicKey, destroyed: false};
}

export function forgetMatchPeerIdentity(identity) {
  if (!identity || typeof identity !== "object") return;
  identity.destroyed = true;
  identity.privateKey = null;
  if (identity.publicKey instanceof Uint8Array) identity.publicKey.fill(0);
}

export async function deriveMatchPeerKeyFromIdentity(identity, peerPublicKey,
                                                     transcriptDigest, context,
                                                     provider = globalThis.crypto) {
  if (!provider?.subtle || !identity || identity.destroyed !== false ||
      !(identity.privateKey instanceof CryptoKey) || !bytes(peerPublicKey, 65) ||
      peerPublicKey[0] !== 4 || !bytes(transcriptDigest, 32) ||
      !keyContextValid(context)) throw new TypeError("invalid match peer identity");
  const peer = await provider.subtle.importKey("raw", peerPublicKey,
    {name: "ECDH", namedCurve: "P-256"}, false, []);
  const shared = new Uint8Array(await provider.subtle.deriveBits(
    {name: "ECDH", public: peer}, identity.privateKey, 256));
  try {
    return await deriveMatchPeerKey(shared, transcriptDigest, context, provider);
  } finally { shared.fill(0); }
}

// Accepts the record returned by derive (or a bare key) and retires both the
// handle and its registry entry, so a later derive of the same inputs starts a
// genuinely fresh key rather than resurrecting a spent sequence space.
export function forgetMatchPeerKey(value) {
  if (!value || typeof value !== "object") return;
  const key = value.key && typeof value.key === "object" ? value.key : value;
  if (typeof key.fingerprint === "string") KEY_REGISTRY.delete(key.fingerprint);
  key.destroyed = true;
  key.handle = null;
}

// Internal: the only seal-window constructor, reachable only from derive. It is
// deliberately not exported — handing callers a way to mint a second window for
// a key is exactly the nonce-reuse seam this module must not have.
function mintSealWindow(key, direction) {
  if (!usableKey(key) || ISSUED_SEAL_KEYS.has(key) || !keyContextValid(direction) ||
      !directionEqual(key.direction, direction))
    throw new TypeError("invalid match peer seal direction");
  const copy = Object.freeze({matchEpoch: direction.matchEpoch,
    sourceEndpointId: direction.sourceEndpointId,
    sourceGeneration: direction.sourceGeneration,
    destinationEndpointId: direction.destinationEndpointId,
    destinationGeneration: direction.destinationGeneration});
  const token = {};
  const state = {key, direction: copy, nextSequence: 1n, ready: true, busy: false};
  Object.defineProperties(token, {
    direction: {value: copy, enumerable: true},
    nextSequence: {get: () => state.nextSequence, enumerable: true},
    ready: {get: () => state.ready, enumerable: true},
  });
  Object.freeze(token);
  SEAL_WINDOWS.set(token, state);
  ISSUED_SEAL_KEYS.add(key);
  return token;
}

function sealWindowValid(window) {
  const state = window && SEAL_WINDOWS.get(window);
  return state && keyContextValid(state.direction) &&
    ((state.ready && u64(state.nextSequence, true)) ||
     (!state.ready && state.nextSequence === 0n)) ? state : null;
}

// Takes the record minted by derive: the key and its one window travel
// together, so there is no way to pair a key with someone else's window.
export async function sealMatchPeerEnvelope(sealing, context, payload,
                                            provider = globalThis.crypto) {
  const key = sealing && typeof sealing === "object" ? sealing.key : null;
  const state = sealWindowValid(sealing && sealing.sealWindow);
  if (!provider?.subtle || !usableKey(key) || !bytes(payload, MATCH_PEER_PAYLOAD_BYTES) ||
      !state || state.key !== key || !state.ready || state.busy ||
      !sendContextValid(context) ||
      !directionEqual(state.direction, context))
    throw new TypeError("invalid match peer envelope");
  const sealed = {...context, sequence: state.nextSequence};
  const aad = header(sealed);
  state.busy = true;
  try {
    const encrypted = new Uint8Array(await provider.subtle.encrypt({name: "AES-GCM",
      iv: nonce(sealed), additionalData: aad, tagLength: 128},
      key.handle, payload));
    if (encrypted.byteLength !== MATCH_PEER_PAYLOAD_BYTES + 16)
      throw new Error("unexpected AES-GCM output length");
    const output = new Uint8Array(MATCH_PEER_ENVELOPE_BYTES);
    output.set(aad);
    output.set(encrypted, MATCH_PEER_HEADER_BYTES);
    if (state.nextSequence === U64_MAX) {
      state.nextSequence = 0n;
      state.ready = false;
    } else {
      state.nextSequence++;
    }
    return output;
  } finally {
    state.busy = false;
  }
}

function replayWindowValid(replay) {
  if (!replay || typeof replay !== "object" ||
      (replay.initialized !== true && replay.initialized !== false) ||
      !u64(replay.greatestSequence) || !u64(replay.seenBitmap)) return false;
  return replay.initialized
    ? replay.greatestSequence !== 0n && (replay.seenBitmap & 1n) === 1n
    : replay.greatestSequence === 0n && replay.seenBitmap === 0n;
}

function replayNext(replay, sequence) {
  if (!replayWindowValid(replay)) return null;
  if (!replay.initialized) return {initialized: true, greatestSequence: sequence,
    seenBitmap: 1n};
  if (sequence > replay.greatestSequence) {
    const distance = sequence - replay.greatestSequence;
    return {initialized: true, greatestSequence: sequence,
      seenBitmap: distance >= 64n ? 1n :
        BigInt.asUintN(64, (replay.seenBitmap << distance) | 1n)};
  }
  const distance = replay.greatestSequence - sequence;
  if (distance >= 64n || (replay.seenBitmap & (1n << distance)) !== 0n) return false;
  return {initialized: true, greatestSequence: replay.greatestSequence,
    seenBitmap: replay.seenBitmap | (1n << distance)};
}

export function createMatchPeerReplayWindow() {
  return {initialized: false, greatestSequence: 0n, seenBitmap: 0n};
}

export function inspectMatchPeerEnvelope(envelope) {
  return parseHeader(envelope);
}

export async function openMatchPeerEnvelope(opening, expected, replay, envelope,
                                            provider = globalThis.crypto) {
  const key = opening && typeof opening === "object" ? opening.key : null;
  if (!provider?.subtle || !usableKey(key) || !keyContextValid(expected))
    return {result: "invalid"};
  const context = parseHeader(envelope);
  if (!context) return {result: "invalid"};
  if (context.matchEpoch !== expected.matchEpoch) return {result: "stale_epoch"};
  if (context.sourceEndpointId !== expected.sourceEndpointId)
    return {result: "wrong_source"};
  if (context.destinationEndpointId !== expected.destinationEndpointId)
    return {result: "wrong_recipient"};
  if (context.sourceGeneration !== expected.sourceGeneration ||
      context.destinationGeneration !== expected.destinationGeneration)
    return {result: "stale_generation"};
  if (!replayWindowValid(replay)) return {result: "invalid"};
  let payload;
  try {
    payload = new Uint8Array(await provider.subtle.decrypt({name: "AES-GCM",
      iv: nonce(context), additionalData: envelope.subarray(0, MATCH_PEER_HEADER_BYTES),
      tagLength: 128}, key.handle,
    envelope.subarray(MATCH_PEER_HEADER_BYTES)));
  } catch {
    return {result: "authentication"};
  }
  const next = replayNext(replay, context.sequence);
  if (next === null) { payload.fill(0); return {result: "invalid"}; }
  if (next === false) { payload.fill(0); return {result: "replay"}; }
  Object.assign(replay, next);
  return {result: "ok", context, payload};
}
