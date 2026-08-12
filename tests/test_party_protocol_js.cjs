"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const protocol = require(path.join(
  __dirname, "..", "dist", "web", "party", "party-protocol.js"));
const fixture = JSON.parse(fs.readFileSync(path.join(
  __dirname, "fixtures", "party", "pad-state-v1.json"), "utf8"));

const encoded = protocol.encode(fixture.packet);
assert.equal(Buffer.from(encoded).toString("hex"), fixture.encodedHex);
assert.deepEqual(protocol.decode(encoded), fixture.packet);

for (let byte = 0; byte < encoded.length; byte++) {
  const mutation = encoded.slice();
  mutation[byte] ^= 1;
  assert.throws(() => protocol.decode(mutation), protocol.PartyProtocolError);
}
assert.throws(() => protocol.decode(encoded.slice(0, -1)), /rejected/);
assert.throws(() => protocol.decode(
  new Uint8Array([...encoded, 0])), /rejected/);

const source = encoded.slice();
const decoded = protocol.decode(source);
source.fill(0);
assert.equal(decoded.connectionSequence, fixture.packet.connectionSequence);
assert.equal(Object.isFrozen(decoded), true);

console.log("party_protocol_js: PASS");
