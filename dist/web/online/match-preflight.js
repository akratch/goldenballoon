// Fixed browser half of platform/net/match_preflight.c's MPF1 attestation.
// The bytes travel only over a carrier that already authenticated endpointId.
export const MATCH_PREFLIGHT_ATTESTATION_BYTES = 124;
export const MATCH_PREFLIGHT_ROM_VERIFIED = 0x01;
export const MATCH_PREFLIGHT_PHRASE_CONFIRMED = 0x02;
export const MATCH_PREFLIGHT_CHANNELS_READY = 0x04;
export const MATCH_PREFLIGHT_ALL_FLAGS = 0x07;
export const MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES = 6;
export const MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES = 58;
export const MATCH_PREFLIGHT_FRAGMENT_COUNT = 3;
export const MATCH_PREFLIGHT_FRAGMENT_PAYLOAD_BYTES = 64;

const U32_MAX = 0xffff_ffff;
const U64_MAX = (1n << 64n) - 1n;
const GRAPH_DOMAIN = new TextEncoder().encode(
  "golden-balloon-match-peer-graph-v1");

function u32(value) {
  return Number.isInteger(value) && value >= 1 && value <= U32_MAX;
}

function u64(value) {
  return typeof value === "bigint" && value >= 1n && value <= U64_MAX;
}

function u64zero(value) {
  return typeof value === "bigint" && value >= 0n && value <= U64_MAX;
}

function digest(value) {
  return value instanceof Uint8Array && value.byteLength === 32;
}

function valid(value) {
  return value && u32(value.matchEpoch) && u32(value.connectionGeneration) &&
    u32(value.sequence) && u64(value.endpointId) &&
    Number.isInteger(value.flags) && value.flags >= 0 &&
    value.flags <= MATCH_PREFLIGHT_ALL_FLAGS &&
    digest(value.descriptorDigest) && digest(value.transcriptDigest) &&
    digest(value.graphDigest);
}

export async function digestMatchPreflightGraph(graph,
                                                provider = globalThis.crypto) {
  const endpoints = graph?.endpoints;
  if (!provider?.subtle || graph?.protocolVersion !== 1 ||
      !u32(graph?.matchEpoch) || !Array.isArray(endpoints) ||
      endpoints.length < 2 || endpoints.length > 4 || endpoints.some((endpoint,
        index) => !u64(endpoint?.endpointId) || !u32(endpoint?.generation) ||
        !Number.isInteger(endpoint?.reachableMask) || endpoint.reachableMask < 0 ||
        endpoint.reachableMask >= (1 << endpoints.length) ||
        (endpoint.reachableMask & (1 << index)) !== 0) ||
      new Set(endpoints.map(endpoint => endpoint.endpointId)).size !== endpoints.length)
    throw new TypeError("invalid match preflight graph");
  const sorted = endpoints.map((endpoint, index) => ({endpoint, index}))
    .sort((left, right) => left.endpoint.endpointId < right.endpoint.endpointId ? -1 :
      left.endpoint.endpointId > right.endpoint.endpointId ? 1 : 0);
  const output = new Uint8Array(GRAPH_DOMAIN.length + 4 + 4 + 1 +
    sorted.length * 13);
  output.set(GRAPH_DOMAIN);
  const view = new DataView(output.buffer);
  let offset = GRAPH_DOMAIN.length;
  view.setUint32(offset, graph.protocolVersion, false); offset += 4;
  view.setUint32(offset, graph.matchEpoch, false); offset += 4;
  output[offset++] = sorted.length;
  for (const source of sorted) {
    view.setBigUint64(offset, source.endpoint.endpointId, false); offset += 8;
    view.setUint32(offset, source.endpoint.generation, false); offset += 4;
    let canonicalMask = 0;
    sorted.forEach((destination, canonicalIndex) => {
      if ((source.endpoint.reachableMask & (1 << destination.index)) !== 0)
        canonicalMask |= 1 << canonicalIndex;
    });
    output[offset++] = canonicalMask;
  }
  if (offset !== output.byteLength)
    throw new Error("match preflight graph size mismatch");
  return new Uint8Array(await provider.subtle.digest("SHA-256", output));
}

export function encodeMatchPreflightAttestation(value) {
  if (!valid(value)) throw new TypeError("invalid match preflight attestation");
  const output = new Uint8Array(MATCH_PREFLIGHT_ATTESTATION_BYTES);
  output.set([0x4d, 0x50, 0x46, 0x31, 0x01, value.flags], 0);
  const view = new DataView(output.buffer);
  view.setUint32(8, value.matchEpoch, false);
  view.setUint32(12, value.connectionGeneration, false);
  view.setUint32(16, value.sequence, false);
  view.setBigUint64(20, value.endpointId, false);
  output.set(value.descriptorDigest, 28);
  output.set(value.transcriptDigest, 60);
  output.set(value.graphDigest, 92);
  return output;
}

export function decodeMatchPreflightAttestation(bytes) {
  if (!(bytes instanceof Uint8Array) ||
      bytes.byteLength !== MATCH_PREFLIGHT_ATTESTATION_BYTES ||
      bytes[0] !== 0x4d || bytes[1] !== 0x50 || bytes[2] !== 0x46 ||
      bytes[3] !== 0x31 || bytes[4] !== 0x01 || bytes[6] !== 0 ||
      bytes[7] !== 0) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const value = {
    matchEpoch: view.getUint32(8, false),
    connectionGeneration: view.getUint32(12, false),
    sequence: view.getUint32(16, false),
    endpointId: view.getBigUint64(20, false),
    descriptorDigest: bytes.slice(28, 60),
    transcriptDigest: bytes.slice(60, 92),
    graphDigest: bytes.slice(92, 124),
    flags: bytes[5],
  };
  return valid(value) ? value : null;
}

function fragmentDataSize(index) {
  return Math.min(MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES,
    MATCH_PREFLIGHT_ATTESTATION_BYTES - index * MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES);
}

export function encodeMatchPreflightFragments(value) {
  const encoded = encodeMatchPreflightAttestation(value);
  return Array.from({length: MATCH_PREFLIGHT_FRAGMENT_COUNT}, (_, index) => {
    const payload = new Uint8Array(MATCH_PREFLIGHT_FRAGMENT_PAYLOAD_BYTES);
    const view = new DataView(payload.buffer);
    view.setUint32(0, value.sequence, false);
    payload[4] = index;
    payload[5] = MATCH_PREFLIGHT_FRAGMENT_COUNT;
    const offset = index * MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    payload.set(encoded.subarray(offset, offset + fragmentDataSize(index)),
      MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES);
    return payload;
  });
}

function directionValid(value) {
  return value && u32(value.matchEpoch) && u64(value.sourceEndpointId) &&
    u32(value.sourceGeneration) && u64(value.destinationEndpointId) &&
    u32(value.destinationGeneration) &&
    value.sourceEndpointId !== value.destinationEndpointId;
}

function directionEqual(left, right) {
  return left.matchEpoch === right.matchEpoch &&
    left.sourceEndpointId === right.sourceEndpointId &&
    left.sourceGeneration === right.sourceGeneration &&
    left.destinationEndpointId === right.destinationEndpointId &&
    left.destinationGeneration === right.destinationGeneration;
}

function fragmentContextValid(value) {
  return directionValid(value) && u64zero(value.intermediateEndpointId) &&
    u64(value.sequence) && value.payloadType === 1 &&
    (value.intermediateEndpointId === 0n ||
      (value.intermediateEndpointId !== value.sourceEndpointId &&
       value.intermediateEndpointId !== value.destinationEndpointId));
}

export function createMatchPreflightFragmentState(authenticatedDirection) {
  if (!directionValid(authenticatedDirection))
    throw new TypeError("invalid match preflight fragment direction");
  const direction = {matchEpoch: authenticatedDirection.matchEpoch,
    sourceEndpointId: authenticatedDirection.sourceEndpointId,
    sourceGeneration: authenticatedDirection.sourceGeneration,
    destinationEndpointId: authenticatedDirection.destinationEndpointId,
    destinationGeneration: authenticatedDirection.destinationGeneration};
  const state = {sequence: 0, presentMask: 0,
    encoded: new Uint8Array(MATCH_PREFLIGHT_ATTESTATION_BYTES)};
  Object.defineProperty(state, "direction", {value: Object.freeze(direction),
    enumerable: true, writable: false, configurable: false});
  return state;
}

function commitFragmentState(state, next) {
  state.sequence = next.sequence;
  state.presentMask = next.presentMask;
  state.encoded = next.encoded;
}

function fragmentStateValid(state) {
  if (!state || !directionValid(state.direction) ||
      !(state.sequence === 0 || u32(state.sequence)) ||
      !Number.isInteger(state.presentMask) || state.presentMask < 0 ||
      state.presentMask >= (1 << MATCH_PREFLIGHT_FRAGMENT_COUNT) ||
      !(state.encoded instanceof Uint8Array) ||
      state.encoded.byteLength !== MATCH_PREFLIGHT_ATTESTATION_BYTES)
    return false;
  if (state.sequence === 0)
    return state.presentMask === 0 && !state.encoded.some(byte => byte !== 0);
  if (state.presentMask === 0) return false;
  for (let index = 0; index < MATCH_PREFLIGHT_FRAGMENT_COUNT; index++) {
    if ((state.presentMask & (1 << index)) !== 0) continue;
    const offset = index * MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    if (state.encoded.subarray(offset, offset + fragmentDataSize(index))
      .some(byte => byte !== 0)) return false;
  }
  if (state.presentMask === (1 << MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1) {
    const decoded = decodeMatchPreflightAttestation(state.encoded);
    if (!decoded || decoded.sequence !== state.sequence) return false;
  }
  return true;
}

export function submitMatchPreflightFragment(state, authenticatedContext,
                                             payload) {
  if (!fragmentStateValid(state) ||
      !fragmentContextValid(authenticatedContext) ||
      !(payload instanceof Uint8Array) ||
      payload.byteLength !== MATCH_PREFLIGHT_FRAGMENT_PAYLOAD_BYTES)
    return {result: "invalid"};
  if (!directionEqual(state.direction, authenticatedContext))
    return {result: "context_mismatch"};
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const sequence = view.getUint32(0, false);
  const index = payload[4];
  if (!u32(sequence) || index >= MATCH_PREFLIGHT_FRAGMENT_COUNT ||
      payload[5] !== MATCH_PREFLIGHT_FRAGMENT_COUNT)
    return {result: "invalid"};
  const count = fragmentDataSize(index);
  if (payload.subarray(MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES + count)
    .some(byte => byte !== 0)) return {result: "invalid"};
  if (state.sequence !== 0 && sequence < state.sequence)
    return {result: "stale_sequence"};
  const next = sequence > state.sequence ?
    createMatchPreflightFragmentState(state.direction) :
    {direction: {...state.direction}, sequence: state.sequence,
      presentMask: state.presentMask,
      encoded: state.encoded.slice()};
  if (sequence > state.sequence) next.sequence = sequence;
  const offset = index * MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
  const data = payload.subarray(MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES,
    MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES + count);
  if ((next.presentMask & (1 << index)) !== 0) {
    const prior = next.encoded.subarray(offset, offset + count);
    return {result: prior.every((byte, position) => byte === data[position])
      ? "duplicate" : "conflict"};
  }
  next.encoded.set(data, offset);
  next.presentMask |= 1 << index;
  if (next.presentMask !== (1 << MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1) {
    commitFragmentState(state, next);
    return {result: "accepted"};
  }
  const attestation = decodeMatchPreflightAttestation(next.encoded);
  if (!attestation || attestation.sequence !== sequence)
    return {result: "invalid"};
  if (attestation.matchEpoch !== state.direction.matchEpoch ||
      attestation.endpointId !== state.direction.sourceEndpointId ||
      attestation.connectionGeneration !== state.direction.sourceGeneration)
    return {result: "context_mismatch"};
  commitFragmentState(state, next);
  return {result: "complete", attestation};
}
