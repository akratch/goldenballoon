import {env} from "cloudflare:workers";
import {SELF, evictDurableObject, runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import type {Env} from "../src/types";

const origin = "https://party.example.invalid";
const compatibility = {protocolVersion: 1,
  buildId: Array.from({length: 16}, (_, i) => i + 1),
  gameplayDigest: Array.from({length: 32}, (_, i) => 128 + i),
  romRevision: 1, cadenceHz: 30};

async function post(path: string, value: unknown, credential = "") {
  const headers = new Headers({origin, "content-type": "application/json"});
  if (credential) headers.set("authorization", `Bearer ${credential}`);
  return SELF.fetch(`https://party.test${path}`, {method: "POST", headers,
    body: JSON.stringify(value)});
}

async function roomPair() {
  const created = await post("/api/match/create", {compatibility, seatCount: 1});
  expect(created.status).toBe(201);
  const host = await created.json() as Record<string, any>;
  const capability = new URL(host.inviteUrl).hash.slice("#match=".length);
  const joined = await post("/api/match/join", {capability, compatibility, seatCount: 1});
  expect(joined.status).toBe(201);
  return {host, guest: await joined.json() as Record<string, any>};
}

function nextMessage(socket: WebSocket, label: string): Promise<Record<string, any>> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label} message timeout`)), 2000);
    socket.addEventListener("message", event => {
      clearTimeout(timer);
      resolve(JSON.parse(String(event.data)) as Record<string, any>);
    }, {once: true});
  });
}

function nextClose(socket: WebSocket, label: string): Promise<CloseEvent> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label} close timeout`)), 2000);
    socket.addEventListener("close", event => { clearTimeout(timer); resolve(event); },
      {once: true});
  });
}

async function connectSignal(room: Record<string, any>, withOrigin = true) {
  const headers: Record<string, string> = {upgrade: "websocket",
    "sec-websocket-protocol":
      `gb-match-signal-v1, gb-match.${room.credential}`};
  if (withOrigin) headers.origin = origin;
  const response = await SELF.fetch(
    `https://party.test/api/match/${room.roomId}/signal`, {headers});
  expect(response.status).toBe(101);
  expect(response.headers.get("sec-websocket-protocol")).toBe("gb-match-signal-v1");
  const socket = response.webSocket!;
  const welcome = nextMessage(socket, "signal welcome");
  socket.accept();
  return {socket, welcome: await welcome};
}

function publicKey(): string {
  const raw = new Uint8Array(65);
  raw[0] = 4;
  raw[64] = 1;
  let binary = "";
  for (const byte of raw) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function signal(type: string, sequence: number, target: Record<string, any>,
                generation: number, extra: Record<string, unknown>) {
  return {protocolVersion: 1, type, sequence,
    toEndpointId: target.endpointId, toConnectionGeneration: generation, ...extra};
}

describe("MatchRoom authenticated signaling relay", () => {
  it("relays only to the authenticated target across hibernation", async () => {
    const {host, guest} = await roomPair();
    const hostSignal = await connectSignal(host);
    expect(hostSignal.welcome).toEqual({protocolVersion: 1, type: "signal_welcome",
      endpointId: host.endpointId, connectionGeneration: 1, peers: []});
    const hostPresence = nextMessage(hostSignal.socket, "guest presence");
    const guestSignal = await connectSignal(guest);
    expect(guestSignal.welcome).toEqual({protocolVersion: 1, type: "signal_welcome",
      endpointId: guest.endpointId, connectionGeneration: 1,
      peers: [{endpointId: host.endpointId, connectionGeneration: 1}]});
    expect(await hostPresence).toEqual({protocolVersion: 1, type: "peer_presence",
      endpointId: guest.endpointId, connectionGeneration: 1, present: true});

    const bindings = env as unknown as Env;
    const stub = bindings.MATCH_ROOMS.get(bindings.MATCH_ROOMS.idFromName(host.roomId));
    await evictDurableObject(stub, {webSockets: "hibernate"});

    const forwarded = nextMessage(guestSignal.socket, "forwarded hello");
    hostSignal.socket.send(JSON.stringify(signal("peer_hello", 1, guest, 1,
      {publicKey: publicKey()})));
    expect(await forwarded).toEqual({protocolVersion: 1, type: "peer_hello", sequence: 1,
      publicKey: publicKey(), toEndpointId: guest.endpointId,
      toConnectionGeneration: 1, fromEndpointId: host.endpointId,
      fromConnectionGeneration: 1});

    const stored = await runInDurableObject(stub, async (_instance, state) =>
      JSON.stringify(Object.fromEntries(await state.storage.list())));
    expect(stored).not.toContain(publicKey());
    expect(stored).not.toContain("v=0");

    const unavailable = nextMessage(hostSignal.socket, "stale target generation");
    hostSignal.socket.send(JSON.stringify(signal("webrtc_offer", 2, guest, 2,
      {sdp: "v=0\r\n"})));
    expect(await unavailable).toEqual({protocolVersion: 1, type: "signal_error",
      sequence: 2, toEndpointId: guest.endpointId, toConnectionGeneration: 2,
      error: "peer_unavailable"});

    const rejected = nextClose(hostSignal.socket, "spoofed sender");
    hostSignal.socket.send(JSON.stringify({...signal("peer_hello", 3, guest, 1,
      {publicKey: publicKey()}), fromEndpointId: guest.endpointId}));
    expect(await rejected).toMatchObject({code: 4003, reason: "invalid_signaling"});
    guestSignal.socket.close(1000, "test_complete");
  }, 10_000);

  it("increments generations, replaces duplicate sockets and supports native launchers", async () => {
    const {host, guest} = await roomPair();
    const hostSignal = await connectSignal(host, false);
    const firstPresence = nextMessage(hostSignal.socket, "first guest presence");
    const first = await connectSignal(guest);
    await firstPresence;
    const replacedClose = nextClose(first.socket, "replaced guest");
    const nextPresence = nextMessage(hostSignal.socket, "replacement guest presence");
    const laterPresence: Record<string, any>[] = [];
    hostSignal.socket.addEventListener("message", event => {
      laterPresence.push(JSON.parse(String(event.data)) as Record<string, any>);
    });
    const replacement = await connectSignal(guest);
    expect(replacement.welcome).toMatchObject({connectionGeneration: 2,
      peers: [{endpointId: host.endpointId, connectionGeneration: 1}]});
    expect(await replacedClose).toMatchObject({code: 4001, reason: "connection_replaced"});
    expect(await nextPresence).toEqual({protocolVersion: 1, type: "peer_presence",
      endpointId: guest.endpointId, connectionGeneration: 2, present: true});
    await new Promise(resolve => setTimeout(resolve, 25));
    expect(laterPresence.filter(item => item.type === "peer_presence" &&
      item.endpointId === guest.endpointId && item.present === false)).toEqual([]);
    hostSignal.socket.close(1000, "test_complete");
    replacement.socket.close(1000, "test_complete");
  });

  it("canonicalizes peer snapshots during overlapping replacements", async () => {
    const {host, guest} = await roomPair();
    const hostSignal = await connectSignal(host);
    const hostPresence = nextMessage(hostSignal.socket, "initial guest presence");
    const guestSignal = await connectSignal(guest);
    await hostPresence;

    const [nextHost, nextGuest] = await Promise.all([
      connectSignal(host), connectSignal(guest),
    ]);
    for (const [self, other, welcome] of [
      [host.endpointId, guest.endpointId, nextHost.welcome],
      [guest.endpointId, host.endpointId, nextGuest.welcome],
    ] as const) {
      expect(welcome.endpointId).toBe(self);
      expect(welcome.peers).toHaveLength(1);
      expect(welcome.peers[0].endpointId).toBe(other);
      expect(new Set(welcome.peers.map((peer: Record<string, any>) =>
        peer.endpointId)).size).toBe(welcome.peers.length);
    }
    nextHost.socket.close(1000, "test_complete");
    nextGuest.socket.close(1000, "test_complete");
  }, 10_000);

  it("revokes messages queued by a superseded source generation", async () => {
    const {host, guest} = await roomPair();
    const hostSignal = await connectSignal(host);
    const hostPresence = nextMessage(hostSignal.socket, "guest presence");
    const guestSignal = await connectSignal(guest);
    await hostPresence;

    const bindings = env as unknown as Env;
    const stub = bindings.MATCH_ROOMS.get(bindings.MATCH_ROOMS.idFromName(host.roomId));
    await runInDurableObject(stub, async (_instance, state) => {
      await state.storage.put(`signal-sequence:${host.endpointId}`, 2);
    });
    const unexpected: Record<string, any>[] = [];
    guestSignal.socket.addEventListener("message", event => {
      unexpected.push(JSON.parse(String(event.data)) as Record<string, any>);
    });
    const staleClose = nextClose(hostSignal.socket, "superseded source");
    hostSignal.socket.send(JSON.stringify(signal("peer_hello", 1, guest, 1,
      {publicKey: publicKey()})));
    expect(await staleClose).toMatchObject({code: 4001, reason: "connection_replaced"});
    await new Promise(resolve => setTimeout(resolve, 25));
    expect(unexpected).toEqual([]);
    guestSignal.socket.close(1000, "test_complete");
  });

  it("rejects malformed authentication and binary signaling independently of state sockets",
    async () => {
      const {host} = await roomPair();
      for (const protocols of [
        `gb-match.${host.credential}`,
        `gb-match-signal-v1, gb-match-signal-v1, gb-match.${host.credential}`,
        `gb-match-v1, gb-match.${host.credential}`,
        `gb-match-signal-v1, gb-match.${"A".repeat(43)}`,
      ]) {
        const response = await SELF.fetch(
          `https://party.test/api/match/${host.roomId}/signal`, {headers: {origin,
            upgrade: "websocket", "sec-websocket-protocol": protocols}});
        expect(response.status).toBe(401);
      }
      const anotherResponse = await post("/api/match/create",
        {compatibility, seatCount: 1});
      const another = await anotherResponse.json() as Record<string, any>;
      const crossRoom = await SELF.fetch(
        `https://party.test/api/match/${host.roomId}/signal`, {headers: {origin,
          upgrade: "websocket", "sec-websocket-protocol":
            `gb-match-signal-v1, gb-match.${another.credential}`}});
      expect(crossRoom.status).toBe(401);
      const connected = await connectSignal(host);
      const binaryClose = nextClose(connected.socket, "binary signaling");
      connected.socket.send(new Uint8Array([1, 2, 3]).buffer);
      expect(await binaryClose).toMatchObject({code: 4003, reason: "signaling_text_only"});

      const state = await SELF.fetch(
        `https://party.test/api/match/${host.roomId}/connect`, {headers: {origin,
          upgrade: "websocket",
          "sec-websocket-protocol": `gb-match-v1, gb-match.${host.credential}`}});
      expect(state.status).toBe(101);
      const stateSocket = state.webSocket!;
      const initial = nextMessage(stateSocket, "state initial");
      stateSocket.accept();
      expect(await initial).toMatchObject({type: "match_state"});
      const stateClose = nextClose(stateSocket, "state command rejection");
      stateSocket.send("peer_hello");
      expect(await stateClose).toMatchObject({code: 4003,
        reason: "commands_use_authenticated_http"});
    });

  it("closes replayed sequences, spoofed senders and oversized UTF-8 frames", async () => {
    const {host, guest} = await roomPair();
    const guestSignal = await connectSignal(guest);
    const hostPresence = nextMessage(guestSignal.socket, "host presence");
    const hostSignal = await connectSignal(host);
    await hostPresence;
    const unavailable = nextMessage(hostSignal.socket, "first unavailable target");
    const staleTarget = signal("peer_end", 1, guest, 2, {reason: "restart"});
    hostSignal.socket.send(JSON.stringify(staleTarget));
    expect(await unavailable).toMatchObject({type: "signal_error", sequence: 1,
      error: "peer_unavailable"});
    const replayClose = nextClose(hostSignal.socket, "replayed sequence");
    hostSignal.socket.send(JSON.stringify(staleTarget));
    expect(await replayClose).toMatchObject({code: 4003, reason: "invalid_sequence"});

    const replacementPresence = nextMessage(guestSignal.socket, "replacement presence");
    const replacement = await connectSignal(host);
    await replacementPresence;
    const spoofClose = nextClose(replacement.socket, "spoofed sender field");
    replacement.socket.send(JSON.stringify({...signal("peer_hello", 1, guest, 1,
      {publicKey: publicKey()}), fromEndpointId: guest.endpointId}));
    expect(await spoofClose).toMatchObject({code: 4003, reason: "invalid_signaling"});

    const oversizePresence = nextMessage(guestSignal.socket, "oversize replacement");
    const oversize = await connectSignal(host);
    await oversizePresence;
    const oversizedClose = nextClose(oversize.socket, "oversized UTF-8");
    oversize.socket.send("é".repeat(32 * 1024 + 1));
    expect(await oversizedClose).toMatchObject({code: 4009, reason: "message_too_large"});
    guestSignal.socket.close(1000, "test_complete");
  });
});
