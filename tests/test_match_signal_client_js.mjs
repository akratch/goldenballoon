import assert from "node:assert/strict";
import fs from "node:fs";
import {createMatchSignalClient} from
  "../dist/web/online/match-signal-client.js";

const onlineRoomSource = fs.readFileSync(
  new URL("../dist/web/online/online-room.js", import.meta.url), "utf8");
const releasePolicySource = fs.readFileSync(
  new URL("../dist/web/online/online-control-config.js", import.meta.url), "utf8");
assert(!onlineRoomSource.includes("match-signal-client"),
  "disabled Online Room must not import or fetch signaling code");
assert.match(releasePolicySource, /enabled:\s*false/,
  "publisher policy must remain disabled during signaling foundation work");

class FakeSocket {
  static instances = [];
  constructor(url, protocols) {
    this.url = url;
    this.protocols = protocols;
    this.readyState = 0;
    this.protocol = protocols[0];
    this.sent = [];
    this.listeners = new Map();
    FakeSocket.instances.push(this);
  }
  addEventListener(type, callback) {
    const list = this.listeners.get(type) || [];
    list.push(callback);
    this.listeners.set(type, list);
  }
  emit(type, event = {}) {
    for (const callback of this.listeners.get(type) || []) callback(event);
  }
  open() { this.readyState = 1; this.emit("open"); }
  message(value) { this.emit("message", {data: JSON.stringify(value)}); }
  send(value) { this.sent.push(value); }
  close(code = 1000, reason = "") {
    if (this.readyState === 3) return;
    this.readyState = 3;
    this.code = code;
    this.reason = reason;
    this.emit("close", {code, reason});
  }
}

function key() {
  const raw = new Uint8Array(65);
  raw[0] = 4; raw[64] = 1;
  return Buffer.from(raw).toString("base64url");
}

const events = [];
const states = [];
const client = createMatchSignalClient({roomId: "R".repeat(22), endpointId: "1",
  credential: "C".repeat(43), pageOrigin: "https://party.test/",
  serviceOrigin: "https://party.test/", WebSocketImpl: FakeSocket,
  onEvent: value => events.push(value), onState: value => states.push(value)});
const connecting = client.connect();
const socket = FakeSocket.instances.at(-1);
assert.equal(socket.url, `wss://party.test/api/match/${"R".repeat(22)}/signal`);
assert.deepEqual(socket.protocols,
  ["gb-match-signal-v1", `gb-match.${"C".repeat(43)}`]);
assert(!socket.url.includes("C".repeat(43)), "credential must not enter URL/log surface");
socket.open();
socket.message({protocolVersion: 1, type: "signal_welcome", endpointId: "1",
  connectionGeneration: 7, peers: [{endpointId: "2", connectionGeneration: 3}]});
assert.deepEqual(await connecting, {phase: "open", connectionGeneration: 7,
  nextSequence: 1, connected: true});
assert.equal(events[0].type, "signal_welcome");

assert.equal(client.send({type: "peer_hello", toEndpointId: "2",
  toConnectionGeneration: 3, publicKey: key()}), 1);
assert.deepEqual(JSON.parse(socket.sent[0]), {protocolVersion: 1, type: "peer_hello",
  sequence: 1, toEndpointId: "2", toConnectionGeneration: 3, publicKey: key()});
assert.throws(() => client.send({type: "peer_hello", toEndpointId: "2",
  toConnectionGeneration: 3, publicKey: key(), fromEndpointId: "9"}),
  /invalid match signal message/);
assert.throws(() => client.send({type: "peer_hello", toEndpointId: "2",
  toConnectionGeneration: 4, publicKey: key()}),
  error => error.code === "signal_peer_unavailable");
socket.message({protocolVersion: 1, type: "signal_error", sequence: 1,
  toEndpointId: "2", toConnectionGeneration: 3, error: "peer_unavailable"});
assert.equal(events.at(-1).type, "signal_error");

socket.message({protocolVersion: 1, type: "webrtc_offer", sequence: 9,
  toEndpointId: "1", toConnectionGeneration: 7, fromEndpointId: "2",
  fromConnectionGeneration: 3, sdp: "v=0\r\n"});
assert.equal(events.at(-1).type, "webrtc_offer");
socket.message({protocolVersion: 1, type: "peer_presence", endpointId: "2",
  connectionGeneration: 4, present: true});
assert.equal(events.at(-1).type, "peer_presence");

// A stale target generation from the service is a contract violation, not an
// event that could be accidentally applied to the fresh peer connection.
socket.message({protocolVersion: 1, type: "webrtc_answer", sequence: 10,
  toEndpointId: "1", toConnectionGeneration: 6, fromEndpointId: "2",
  fromConnectionGeneration: 4, sdp: "v=0\r\n"});
assert.equal(client.snapshot().phase, "failed");
assert.equal(socket.code, 4003);
assert(states.some(value => value.phase === "open") &&
  states.at(-1).phase === "failed");

assert.throws(() => createMatchSignalClient({roomId: "R".repeat(22), endpointId: "1",
  credential: "C".repeat(43), pageOrigin: "https://party.test/",
  serviceOrigin: "https://evil.test/", WebSocketImpl: FakeSocket}),
  /cross-origin signaling refused/);

const rollbackEvents = [];
const rollbackClient = createMatchSignalClient({roomId: "T".repeat(22), endpointId: "1",
  credential: "E".repeat(43), pageOrigin: "https://party.test/",
  WebSocketImpl: FakeSocket, onEvent: value => rollbackEvents.push(value)});
const rollbackConnecting = rollbackClient.connect();
const rollbackSocket = FakeSocket.instances.at(-1);
rollbackSocket.open();
rollbackSocket.message({protocolVersion: 1, type: "signal_welcome", endpointId: "1",
  connectionGeneration: 1, peers: [{endpointId: "2", connectionGeneration: 8}]});
await rollbackConnecting;
rollbackSocket.message({protocolVersion: 1, type: "peer_presence", endpointId: "2",
  connectionGeneration: 8, present: false});
rollbackSocket.message({protocolVersion: 1, type: "peer_presence", endpointId: "2",
  connectionGeneration: 7, present: true});
assert.equal(rollbackClient.snapshot().phase, "failed",
  "absent peers must retain a generation high-water mark");
rollbackSocket.message({protocolVersion: 1, type: "signal_welcome", endpointId: "1",
  connectionGeneration: 9, peers: []});
assert.equal(rollbackClient.snapshot().phase, "failed",
  "late messages must not revive a failed client");

// A relay that invents endpoint identities must not grow the append-only
// generation high-water map without bound. The client fails closed at the cap
// rather than evicting a mark, because evicting one reopens the generation
// rollback the mark exists to refuse.
const floodClient = createMatchSignalClient({roomId: "U".repeat(22), endpointId: "1",
  credential: "F".repeat(43), pageOrigin: "https://party.test/",
  WebSocketImpl: FakeSocket});
const floodConnecting = floodClient.connect();
const floodSocket = FakeSocket.instances.at(-1);
floodSocket.open();
floodSocket.message({protocolVersion: 1, type: "signal_welcome", endpointId: "1",
  connectionGeneration: 1, peers: []});
await floodConnecting;
let admittedIdentities = 0;
for (let index = 0; index < 500 && floodClient.snapshot().phase === "open"; index++) {
  floodSocket.message({protocolVersion: 1, type: "peer_presence",
    endpointId: String(1000 + index), connectionGeneration: 1, present: true});
  if (floodClient.snapshot().phase === "open") admittedIdentities++;
}
assert.equal(floodClient.snapshot().phase, "failed",
  "an unbounded identity flood must fail the signaling socket closed");
assert.ok(admittedIdentities <= 64,
  `generation tracking must stay bounded (admitted ${admittedIdentities})`);
// The bound is a real ceiling, not an accidental early failure on message 1.
assert.ok(admittedIdentities >= 60,
  `a legal-sized room must not trip the bound (admitted ${admittedIdentities})`);

class WrongProtocolSocket extends FakeSocket {
  constructor(url, protocols) {
    super(url, protocols);
    this.protocol = protocols[1];
  }
}
const protocolClient = createMatchSignalClient({roomId: "U".repeat(22), endpointId: "1",
  credential: "F".repeat(43), pageOrigin: "https://party.test/",
  WebSocketImpl: WrongProtocolSocket});
const protocolPending = protocolClient.connect();
protocolPending.catch(() => {});
const protocolSocket = FakeSocket.instances.at(-1);
protocolSocket.open();
await assert.rejects(protocolPending, error => error.code === "invalid_signal_subprotocol");
assert.equal(protocolClient.snapshot().phase, "failed");

const closed = createMatchSignalClient({roomId: "S".repeat(22), endpointId: "9",
  credential: "D".repeat(43), pageOrigin: "http://party.test/",
  WebSocketImpl: FakeSocket});
const pending = closed.connect();
pending.catch(() => {});
closed.close();
await assert.rejects(pending, error => error.code === "signal_client_closed");
assert.equal(closed.snapshot().phase, "closed");

console.log("test_match_signal_client_js: PASS");
