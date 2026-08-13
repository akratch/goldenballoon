import assert from "node:assert/strict";
import {webcrypto} from "node:crypto";
import {createMatchPreflightFragmentState, decodeMatchPreflightAttestation,
  digestMatchPreflightGraph, encodeMatchPreflightAttestation,
  encodeMatchPreflightFragments, submitMatchPreflightFragment,
  MATCH_PREFLIGHT_ALL_FLAGS, MATCH_PREFLIGHT_ATTESTATION_BYTES,
  MATCH_PREFLIGHT_FRAGMENT_COUNT, MATCH_PREFLIGHT_FRAGMENT_PAYLOAD_BYTES}
  from "../dist/web/online/match-preflight.js";

const toHex = value => [...value]
  .map(byte => byte.toString(16).padStart(2, "0")).join("");
const expected = "4d504631010700000000000700000002000000090000000000000014" +
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f" +
  "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf" +
  "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf";
const report = {
  matchEpoch: 7,
  connectionGeneration: 2,
  sequence: 9,
  endpointId: 20n,
  descriptorDigest: Uint8Array.from({length: 32}, (_, index) => index),
  transcriptDigest: Uint8Array.from({length: 32}, (_, index) => 0xa0 + index),
  graphDigest: Uint8Array.from({length: 32}, (_, index) => 0xc0 + index),
  flags: MATCH_PREFLIGHT_ALL_FLAGS,
};
const fragmentDirection = {matchEpoch: 7, sourceEndpointId: 20n,
  sourceGeneration: 2, destinationEndpointId: 10n,
  destinationGeneration: 1};
const fragmentContext = {...fragmentDirection, intermediateEndpointId: 0n,
  sequence: 1n, payloadType: 1};
const submitFragment = (state, payload, context = fragmentContext) =>
  submitMatchPreflightFragment(state, context, payload);
for (const invalidDirection of [
  {...fragmentDirection, matchEpoch: 0},
  {...fragmentDirection, sourceEndpointId: 0n},
  {...fragmentDirection, sourceGeneration: 0},
  {...fragmentDirection, destinationEndpointId: 20n},
  {...fragmentDirection, destinationGeneration: 0},
]) {
  assert.throws(() => createMatchPreflightFragmentState(invalidDirection),
    /invalid match preflight fragment direction/);
}

const encoded = encodeMatchPreflightAttestation(report);
assert.equal(encoded.byteLength, MATCH_PREFLIGHT_ATTESTATION_BYTES);
assert.equal(toHex(encoded), expected);
assert.deepEqual(decodeMatchPreflightAttestation(encoded), report);
assert.equal(decodeMatchPreflightAttestation(encoded.subarray(1)), null);
assert.equal(decodeMatchPreflightAttestation(
  new Uint8Array(MATCH_PREFLIGHT_ATTESTATION_BYTES + 1)), null);

const fragments = encodeMatchPreflightFragments(report);
assert.equal(fragments.length, MATCH_PREFLIGHT_FRAGMENT_COUNT);
assert(fragments.every(value =>
  value.byteLength === MATCH_PREFLIGHT_FRAGMENT_PAYLOAD_BYTES));
assert.deepEqual([...fragments[0].subarray(0, 6)], [0, 0, 0, 9, 0, 3]);
assert.deepEqual([...fragments[2].subarray(0, 6)], [0, 0, 0, 9, 2, 3]);
assert(fragments[2].subarray(14).every(byte => byte === 0));
const fragmentState = createMatchPreflightFragmentState(fragmentDirection);
assert(Object.isFrozen(fragmentState.direction));
assert.throws(() => { fragmentState.direction = {...fragmentDirection,
  sourceEndpointId: 30n}; }, TypeError);
const beforeWrongType = structuredClone(fragmentState);
assert.equal(submitFragment(fragmentState, fragments[2], {...fragmentContext,
  payloadType: 0}).result, "invalid");
assert.deepEqual(fragmentState, beforeWrongType);
const beforeWrongContext = structuredClone(fragmentState);
assert.equal(submitFragment(fragmentState, fragments[2], {...fragmentContext,
  sourceEndpointId: 30n, sourceGeneration: 3}).result, "context_mismatch");
assert.deepEqual(fragmentState, beforeWrongContext);
assert.equal(submitFragment(fragmentState, fragments[2]).result,
  "accepted");
assert.equal(submitFragment(fragmentState, fragments[0]).result,
  "accepted");
const complete = submitFragment(fragmentState, fragments[1]);
assert.equal(complete.result, "complete");
assert.deepEqual(complete.attestation, report);
assert.equal(submitFragment(fragmentState, fragments[1]).result,
  "duplicate");
const beforeConflict = structuredClone(fragmentState);
const conflicting = fragments[1].slice(); conflicting[10] ^= 1;
assert.equal(submitFragment(fragmentState, conflicting).result,
  "conflict");
assert.deepEqual(fragmentState, beforeConflict);
const newer = encodeMatchPreflightFragments({...report, sequence: 10});
assert.equal(submitFragment(fragmentState, newer[0]).result,
  "accepted");
assert.equal(fragmentState.sequence, 10);
assert.equal(fragmentState.presentMask, 1);
assert.equal(submitFragment(fragmentState, fragments[2]).result,
  "stale_sequence");
const invalidPadding = newer[2].slice(); invalidPadding[63] = 1;
const beforePadding = structuredClone(fragmentState);
assert.equal(submitFragment(fragmentState, invalidPadding).result,
  "invalid");
assert.deepEqual(fragmentState, beforePadding);

const forgedReport = {...report, endpointId: 30n, connectionGeneration: 3,
  sequence: 11};
const forgedFragments = encodeMatchPreflightFragments(forgedReport);
const forgedState = createMatchPreflightFragmentState(fragmentDirection);
assert.equal(submitFragment(forgedState, forgedFragments[0]).result, "accepted");
assert.equal(submitFragment(forgedState, forgedFragments[1]).result, "accepted");
const beforeForgedComplete = structuredClone(forgedState);
assert.equal(submitFragment(forgedState, forgedFragments[2]).result,
  "context_mismatch");
assert.deepEqual(forgedState, beforeForgedComplete);

for (const [offset, mask] of [[0, 1], [4, 1], [5, 8], [6, 1], [7, 1]]) {
  const mutated = encoded.slice();
  mutated[offset] ^= mask;
  assert.equal(decodeMatchPreflightAttestation(mutated), null,
    `invalid control byte ${offset}`);
}
for (const [offset, count] of [[8, 4], [12, 4], [16, 4], [20, 8]]) {
  const mutated = encoded.slice();
  mutated.fill(0, offset, offset + count);
  assert.equal(decodeMatchPreflightAttestation(mutated), null,
    `zero required field ${offset}`);
}
for (let index = 28; index < encoded.length; index++) {
  const mutated = encoded.slice();
  mutated[index] ^= 1;
  const decoded = decodeMatchPreflightAttestation(mutated);
  assert(decoded, `digest mutation ${index} remains structurally decodable`);
  assert.notDeepEqual(decoded, report);
}

for (const invalid of [
  {...report, matchEpoch: 0},
  {...report, connectionGeneration: 0},
  {...report, sequence: 0},
  {...report, endpointId: 0n},
  {...report, endpointId: 20},
  {...report, flags: 8},
  {...report, descriptorDigest: new Uint8Array(31)},
  {...report, transcriptDigest: Array(32).fill(0)},
  {...report, graphDigest: new Uint8Array(31)},
]) {
  assert.throws(() => encodeMatchPreflightAttestation(invalid),
    /invalid match preflight attestation/);
}

const graph = {protocolVersion: 1, matchEpoch: 7, endpoints: [
  {endpointId: 10n, generation: 1, reachableMask: 0x06},
  {endpointId: 20n, generation: 2, reachableMask: 0x05},
  {endpointId: 30n, generation: 3, reachableMask: 0x03},
]};
const graphDigest = await digestMatchPreflightGraph(graph, webcrypto);
assert.equal(toHex(graphDigest),
  "04868b4d34d94911193e5a68c1dab2fb1c97d544a04d6b45123d21c7d1639f71");
const reordered = {...graph, endpoints: [
  {endpointId: 30n, generation: 3, reachableMask: 0x06},
  {endpointId: 10n, generation: 1, reachableMask: 0x05},
  {endpointId: 20n, generation: 2, reachableMask: 0x03},
]};
assert.deepEqual(await digestMatchPreflightGraph(reordered, webcrypto), graphDigest,
  "equivalent array orders must share one topology digest");
const changed = structuredClone(reordered);
changed.endpoints[0].reachableMask = 0x02;
assert.notDeepEqual(await digestMatchPreflightGraph(changed, webcrypto), graphDigest,
  "directed reachability must be bound by the topology digest");
for (const invalid of [
  {...graph, protocolVersion: 2},
  {...graph, matchEpoch: 0},
  {...graph, endpoints: graph.endpoints.slice(0, 1)},
  {...graph, endpoints: [graph.endpoints[0], graph.endpoints[0]]},
  {...graph, endpoints: [{...graph.endpoints[0], reachableMask: 1},
    ...graph.endpoints.slice(1)]},
]) {
  await assert.rejects(digestMatchPreflightGraph(invalid, webcrypto),
    /invalid match preflight graph/);
}

console.log("test_match_preflight_js: PASS");
