import {env} from "cloudflare:workers";
import {SELF, evictDurableObject, runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import partyRoom from "../src/party-room.ts?raw";
import matchRoom from "../src/match/match-room.ts?raw";
import partyBudget from "../src/party-budget.ts?raw";
import codeDirectory from "../src/party-code-directory.ts?raw";
import type {Env} from "../src/types";

const origin = "https://party.example.invalid";
const hostPublicKey = "H".repeat(87);
const controllerPublicKey = "C".repeat(87);
const compatibility = {protocolVersion: 1,
  buildId: Array.from({length: 16}, (_, index) => index + 1),
  gameplayDigest: Array.from({length: 32}, (_, index) => 128 + index),
  romRevision: 1, cadenceHz: 30};

async function post(path: string, value?: unknown, headers?: HeadersInit) {
  const requestHeaders = new Headers(headers);
  requestHeaders.set("origin", origin);
  if (value !== undefined) requestHeaders.set("content-type", "application/json");
  const init: RequestInit = {method: "POST", headers: requestHeaders};
  if (value !== undefined) init.body = JSON.stringify(value);
  return SELF.fetch(`https://party.test${path}`, init);
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

function nextMessageMatching(socket: WebSocket, label: string,
                             predicate: (value: Record<string, any>) => boolean) {
  return new Promise<Record<string, any>>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label} timeout`)), 2000);
    const listener = (event: MessageEvent) => {
      const value = JSON.parse(String(event.data)) as Record<string, any>;
      if (!predicate(value)) return;
      clearTimeout(timer);
      socket.removeEventListener("message", listener);
      resolve(value);
    };
    socket.addEventListener("message", listener);
  });
}

function nextClose(socket: WebSocket, label: string): Promise<CloseEvent> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label} close timeout`)), 2000);
    socket.addEventListener("close", event => { clearTimeout(timer); resolve(event); },
      {once: true});
  });
}

const settle = () => new Promise(resolve => setTimeout(resolve, 50));

function matchStub(roomId: string) {
  const bindings = env as unknown as Env;
  return bindings.MATCH_ROOMS.get(bindings.MATCH_ROOMS.idFromName(roomId));
}

function partyStub(roomId: string) {
  const bindings = env as unknown as Env;
  return bindings.PARTY_ROOMS.get(bindings.PARTY_ROOMS.idFromName(roomId));
}

async function matchPair() {
  const created = await post("/api/match/create", {compatibility, seatCount: 1});
  expect(created.status).toBe(201);
  const host = await created.json() as Record<string, any>;
  const capability = new URL(host.inviteUrl).hash.slice("#match=".length);
  const joined = await post("/api/match/join", {capability, compatibility, seatCount: 1});
  expect(joined.status).toBe(201);
  return {host, guest: await joined.json() as Record<string, any>};
}

async function connectMatchState(room: Record<string, any>) {
  const response = await SELF.fetch(
    `https://party.test/api/match/${room.roomId}/connect`, {headers: {origin,
      upgrade: "websocket",
      "sec-websocket-protocol": `gb-match-v1, gb-match.${room.credential}`}});
  expect(response.status).toBe(101);
  const socket = response.webSocket!;
  const initial = nextMessage(socket, "match state");
  socket.accept();
  return {socket, initial: await initial};
}

async function connectMatchSignal(room: Record<string, any>) {
  const response = await SELF.fetch(
    `https://party.test/api/match/${room.roomId}/signal`, {headers: {origin,
      upgrade: "websocket",
      "sec-websocket-protocol": `gb-match-signal-v1, gb-match.${room.credential}`}});
  expect(response.status).toBe(101);
  const socket = response.webSocket!;
  const welcome = nextMessage(socket, "signal welcome");
  socket.accept();
  return {socket, welcome: await welcome};
}

function setCharacter(revision: number, commandId: string) {
  return {protocolVersion: 1, expectedRevision: revision, commandId,
    type: "set_character", value: 0, targetEndpointId: "0"};
}

function liveSocketCount(stub: ReturnType<typeof matchStub> |
                         ReturnType<typeof partyStub>): Promise<number> {
  return runInDurableObject(stub as any,
    async (_instance: unknown, state: DurableObjectState) =>
      state.getWebSockets().length);
}

describe("Workers Free hibernation lifecycle", () => {
  it("keeps match sockets accepted, attached and routed across an eviction",
      async () => {
    const {host, guest} = await matchPair();
    const state = await connectMatchState(host);
    const signal = await connectMatchSignal(host);
    expect(state.initial).toMatchObject({type: "match_state"});
    expect(signal.welcome).toMatchObject({type: "signal_welcome",
      endpointId: host.endpointId, connectionGeneration: 1});

    /* Evicting the object with hibernating sockets is the real lifecycle: only
     * sockets handed to ctx.acceptWebSocket() survive it, and only state that
     * went through serializeAttachment() comes back with them. A socket the
     * object had accept()ed itself, or a room that kept its identity in an
     * instance field, cannot pass anything below this line. */
    const stub = matchStub(host.roomId);
    await evictDurableObject(stub, {webSockets: "hibernate"});
    expect(await liveSocketCount(stub)).toBe(2);

    // The restored state socket still receives authoritative broadcasts.
    const broadcast = nextMessageMatching(state.socket, "post-eviction broadcast",
      value => value.type === "match_state");
    const command = await post(`/api/match/${host.roomId}/command`,
      setCharacter(state.initial.lobby.revision as number, "11"),
      {authorization: `Bearer ${host.credential}`});
    expect(command.status).toBe(200);
    expect((await broadcast).lobby.revision)
      .toBeGreaterThan(state.initial.lobby.revision as number);

    /* Routing after the eviction proves the deserialized attachment, not a
     * surviving instance field, decides what a socket is allowed to do: the
     * state socket is refused as a command channel and the signal socket is
     * still recognised as this endpoint's signaling peer. */
    const refused = nextClose(state.socket, "restored state socket");
    state.socket.send("peer_hello");
    expect(await refused).toMatchObject({code: 4003,
      reason: "commands_use_authenticated_http"});

    const guestSignal = await connectMatchSignal(guest);
    expect(guestSignal.welcome.peers).toEqual([
      {endpointId: host.endpointId, connectionGeneration: 1}]);
    guestSignal.socket.close(1000, "test_complete");
    signal.socket.close(1000, "test_complete");
  }, 15_000);

  it("keeps Phone Party host and controller sockets across an eviction", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    expect(created.status).toBe(201);
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    const response = await SELF.fetch(
      `https://party.test/api/party/${room.roomId}/connect`, {headers: {origin,
        upgrade: "websocket",
        "sec-websocket-protocol": `gb-control-v1, gb-host.${room.hostCredential}`}});
    expect(response.status).toBe(101);
    const host = response.webSocket!;
    const initial = nextMessage(host, "room state");
    host.accept();
    expect(await initial).toMatchObject({type: "room_state", transitionId: 1});

    const pending = nextMessageMatching(host, "pending controller",
      value => value.type === "room_state" && value.controllers.length === 1);
    const redeemed = await post("/api/controller/redeem",
      {capability, protocol: 2, name: "Phone", controllerPublicKey});
    expect(redeemed.status).toBe(201);
    const controller = await redeemed.json() as Record<string, string>;
    await pending;

    const controllerResponse = await SELF.fetch(
      `https://party.test/api/party/${room.roomId}/connect`, {headers: {origin,
        upgrade: "websocket",
        "sec-websocket-protocol": `gb-control-v1, gb-controller.${controller.credential}`}});
    expect(controllerResponse.status).toBe(101);
    const controllerSocket = controllerResponse.webSocket!;
    const controllerInitial = nextMessage(controllerSocket, "controller state");
    controllerSocket.accept();
    expect(await controllerInitial).toMatchObject({type: "controller_state",
      phase: "pending"});

    const stub = partyStub(room.roomId!);
    await evictDurableObject(stub, {webSockets: "hibernate"});
    expect(await liveSocketCount(stub)).toBe(2);

    /* Both roles must survive, and the controller-only message must still be
     * addressed by its restored attachment rather than broadcast to everyone. */
    const hostSaw = nextMessageMatching(host, "approved room state",
      value => value.type === "room_state" &&
        value.controllers.some((item: Record<string, unknown>) =>
          item.phase === "leased" && item.seat === 1));
    const controllerSaw = nextMessageMatching(controllerSocket, "leased controller",
      value => value.type === "controller_state" && value.phase === "leased");
    const approved = await post(`/api/party/${room.roomId}/approve`,
      {controllerId: controller.controllerId, seat: 1},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(approved.status).toBe(200);
    await hostSaw;
    expect(await controllerSaw).toMatchObject({seat: 1});
    host.close(1000, "test_complete");
    controllerSocket.close(1000, "test_complete");
  }, 15_000);

  it("evicts a controller's prior socket when the same identity reconnects",
      async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    expect(created.status).toBe(201);
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    const redeemed = await post("/api/controller/redeem",
      {capability, protocol: 2, name: "Phone", controllerPublicKey});
    expect(redeemed.status).toBe(201);
    const controller = await redeemed.json() as Record<string, string>;

    const connect = async () => {
      const res = await SELF.fetch(
        `https://party.test/api/party/${room.roomId}/connect`, {headers: {origin,
          upgrade: "websocket",
          "sec-websocket-protocol":
            `gb-control-v1, gb-controller.${controller.credential}`}});
      expect(res.status).toBe(101);
      return res.webSocket!;
    };

    const first = await connect();
    const firstReady = nextMessage(first, "first controller state");
    first.accept();
    await firstReady;

    // Reconnecting the same controller identity must close the predecessor so a
    // stale hibernated peer cannot keep receiving relayed signaling.
    const evicted = nextClose(first, "evicted controller");
    const second = await connect();
    const secondReady = nextMessage(second, "second controller state");
    second.accept();
    expect(await secondReady).toMatchObject({type: "controller_state"});

    const close = await evicted;
    expect(close.code).toBe(4000);
    expect(close.reason).toBe("duplicate_controller");
    await settle();
    expect(await liveSocketCount(partyStub(room.roomId!))).toBe(1);
    second.close(1000, "test_complete");
  }, 15_000);

  it("announces signaling absence for a final close and never for a replacement",
      async () => {
    const {host, guest} = await matchPair();
    const hostSignal = await connectMatchSignal(host);
    const arrived = nextMessageMatching(hostSignal.socket, "guest arrival",
      value => value.type === "peer_presence" && value.present === true);
    const guestSignal = await connectMatchSignal(guest);
    expect((await arrived).connectionGeneration).toBe(1);

    /* The close handler owns presence custody: a genuine last close publishes
     * absence for exactly the generation that closed. */
    const absence = nextMessageMatching(hostSignal.socket, "guest absence",
      value => value.type === "peer_presence" && value.present === false);
    guestSignal.socket.close(1000, "guest_left");
    expect(await absence).toMatchObject({endpointId: guest.endpointId,
      connectionGeneration: 1, present: false});

    // A replacement upgrade must never let the superseded close publish absence.
    const returned = nextMessageMatching(hostSignal.socket, "guest return",
      value => value.type === "peer_presence" && value.present === true &&
        value.connectionGeneration === 2);
    const second = await connectMatchSignal(guest);
    await returned;
    const observed: Record<string, any>[] = [];
    hostSignal.socket.addEventListener("message", event => {
      observed.push(JSON.parse(String(event.data)) as Record<string, any>);
    });
    const replaced = nextClose(second.socket, "replaced guest");
    const third = await connectMatchSignal(guest);
    expect(third.welcome.connectionGeneration).toBe(3);
    expect(await replaced).toMatchObject({code: 4001, reason: "connection_replaced"});
    await settle();
    expect(observed.filter(value => value.type === "peer_presence" &&
      value.present === false)).toEqual([]);
    expect(observed.filter(value => value.type === "peer_presence" &&
      value.present === true && value.connectionGeneration === 3)).toHaveLength(1);
    hostSignal.socket.close(1000, "test_complete");
    third.socket.close(1000, "test_complete");
  }, 15_000);

  it("delivers state to healthy peers after a peer socket has died", async () => {
    const {host, guest} = await matchPair();
    const hostState = await connectMatchState(host);
    const guestState = await connectMatchState(guest);
    guestState.socket.close(1000, "peer_gone");
    await settle();

    /* Delivery is best effort only after authority commits: the dead peer must
     * neither fail the command nor stop the surviving peer's broadcast. The
     * helper contract itself is covered by match-delivery.test.ts. */
    const broadcast = nextMessageMatching(hostState.socket, "surviving peer",
      value => value.type === "match_state");
    const command = await post(`/api/match/${host.roomId}/command`,
      setCharacter(hostState.initial.lobby.revision as number, "21"),
      {authorization: `Bearer ${host.credential}`});
    expect(command.status).toBe(200);
    expect((await broadcast).lobby.revision)
      .toBeGreaterThan(hostState.initial.lobby.revision as number);
    hostState.socket.close(1000, "test_complete");
  }, 15_000);

  it("serializes native host commands against concurrent HTTP authority",
      async () => {
    const response = await SELF.fetch("https://party.test/api/party/native-create", {
      headers: {upgrade: "websocket", "sec-websocket-protocol":
        `gb-native-host-v1, gb-control-v1, gb-key.${hostPublicKey}`}});
    expect(response.status).toBe(101);
    const socket = response.webSocket!;
    const transitions: number[] = [];
    socket.addEventListener("message", event => {
      const value = JSON.parse(String(event.data)) as Record<string, any>;
      if (value.type === "room_state") transitions.push(value.transitionId as number);
    });
    const pending = nextMessage(socket, "native bootstrap");
    socket.accept();
    const bootstrap = await pending;
    expect(bootstrap).toMatchObject({type: "native_bootstrap"});
    const capability = new URL(bootstrap.controllerUrl).hash.slice(1);

    const redeem = async (key: string) => {
      const redeemed = await post("/api/controller/redeem",
        {capability, protocol: 2, controllerPublicKey: key.repeat(87)});
      expect(redeemed.status).toBe(201);
      return redeemed.json() as Promise<Record<string, string>>;
    };
    const first = await redeem("A");
    const second = await redeem("B");

    /* The socket and HTTP transports reach the same room through different
     * entry points. Both claim seat 1 at once: whichever loses must lose
     * cleanly, with no split ownership and no reused transition id. */
    socket.send(JSON.stringify({type: "host_command", action: "approve",
      controllerId: first.controllerId, seat: 1}));
    const overHttp = await post(`/api/party/${bootstrap.roomId}/approve`,
      {controllerId: second.controllerId, seat: 1},
      {authorization: `Bearer ${bootstrap.hostCredential}`});
    expect([200, 409]).toContain(overHttp.status);
    await settle();

    const stored = await runInDurableObject(partyStub(bootstrap.roomId),
      async (_instance, state) => state.storage.get<Record<string, any>>("room"));
    const leased = stored!.controllers.filter((item: Record<string, unknown>) =>
      item.phase === "leased" && item.seat === 1);
    expect(leased).toHaveLength(1);
    expect(stored!.controllers.filter((item: Record<string, unknown>) =>
      item.phase === "pending")).toHaveLength(1);
    expect(transitions).toEqual([...new Set(transitions)]);
    expect(transitions).toEqual([...transitions].sort((a, b) => a - b));
    expect(stored!.transitionId).toBe(Math.max(...transitions));
    socket.close(1000, "test_complete");
  }, 15_000);
});

/* The assertions below remain source-text greps. Each states a property of the
 * module that this harness cannot observe: workerd exposes no way to enumerate
 * an object's pending timers, its outbound socket attempts, or the call sites
 * of a helper, and a violation shows up only as billing and duration behaviour
 * on the real platform. Each is therefore paired with a positive control that
 * proves the matcher can still fire, and is stated against a resolved symbol
 * rather than a loose occurrence count. */
describe("hibernation eligibility properties not observable in this harness", () => {
  const durableObjectSources = [
    ["party-room.ts", partyRoom], ["match-room.ts", matchRoom],
    ["party-budget.ts", partyBudget], ["party-code-directory.ts", codeDirectory],
  ] as const;

  it("schedules no wall-clock timer and opens no outbound socket", () => {
    const timer = /\bset(?:Timeout|Interval)\s*\(/;
    const outbound = /new\s+WebSocket\s*\(/;
    const listener = /\.addEventListener\s*\(/;
    // Positive controls: these matchers must be able to fail.
    expect("const handle = setTimeout(() => {}, 1);").toMatch(timer);
    expect("const socket = new WebSocket(url);").toMatch(outbound);
    expect("socket.addEventListener(\"message\", handler);").toMatch(listener);
    for (const [name, text] of durableObjectSources) {
      expect(text, name).not.toMatch(timer);
      expect(text, name).not.toMatch(outbound);
      expect(text, name).not.toMatch(listener);
    }
  });

  it("serializes every Durable Object's read-decide-write behind blockConcurrencyWhile", () => {
    const guard = /this\.ctx\.blockConcurrencyWhile\s*\(/;
    // Positive control: the matcher must be able to fire and to fail.
    expect("return this.ctx.blockConcurrencyWhile(() => this.run());").toMatch(guard);
    expect("await this.ctx.storage.put(key, value);").not.toMatch(guard);
    // Every DO in this service resolves a stored predecessor across an await
    // before writing it back; without the guard a concurrent request races it
    // (code collision, undercounted guess limiter). PartyCodeDirectory was once
    // the lone omission — keep all four covered.
    for (const [name, text] of durableObjectSources) {
      expect(text, name).toMatch(guard);
    }
  });

  it("routes every socket close through the exception-swallowing helper", () => {
    const helpers = [
      ["match-room.ts", matchRoom, "closeSocket"],
      ["party-room.ts", partyRoom, "closePartySocket"],
    ] as const;
    for (const [name, text, helper] of helpers) {
      const start = text.indexOf(`function ${helper}(socket: WebSocket,`);
      expect(start, `${name}: ${helper} is defined`).toBeGreaterThanOrEqual(0);
      const end = text.indexOf("\n}", start);
      expect(end, `${name}: ${helper} body ends`).toBeGreaterThan(start);
      const body = text.slice(start, end);
      // The helper is the one place a raw close may happen, and it swallows.
      expect(body, `${name}: ${helper} closes`).toContain("socket.close(code, reason);");
      expect(body, `${name}: ${helper} swallows`).toMatch(/catch\s*\{/);
      let index = text.indexOf("socket.close(");
      while (index >= 0) {
        expect(index, `${name}: raw close outside ${helper} at ${index}`)
          .toBeGreaterThanOrEqual(start);
        expect(index, `${name}: raw close outside ${helper} at ${index}`)
          .toBeLessThan(end);
        index = text.indexOf("socket.close(", index + 1);
      }
    }
  });
});
