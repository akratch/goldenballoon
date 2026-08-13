import {env} from "cloudflare:workers";
import {SELF, abortAllDurableObjects, runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import {INTERNAL_API_HEADER} from "../src/internal-api";
import {LIMITS, type Env} from "../src/types";

const origin = "https://party.example.invalid";
const hostPublicKey = "H".repeat(87);
const controllerPublicKey = "C".repeat(87);

async function post(path: string, value?: unknown, headers?: HeadersInit) {
  const requestHeaders = new Headers(headers);
  requestHeaders.set("origin", origin);
  if (value !== undefined) requestHeaders.set("content-type", "application/json");
  const init: RequestInit = {method: "POST", headers: requestHeaders};
  if (value !== undefined) init.body = JSON.stringify(value);
  return SELF.fetch(`https://party.test${path}`, init);
}

function nextMessages(socket: WebSocket, count: number): Promise<Record<string, any>[]> {
  return new Promise((resolve, reject) => {
    const values: Record<string, any>[] = [];
    const timer = setTimeout(() => reject(new Error("party socket message timeout")), 2000);
    socket.addEventListener("message", event => {
      values.push(JSON.parse(String(event.data)) as Record<string, any>);
      if (values.length === count) {
        clearTimeout(timer);
        resolve(values);
      }
    });
  });
}

function nextMessageMatching(socket: WebSocket,
                             predicate: (value: Record<string, any>) => boolean) {
  return new Promise<Record<string, any>>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("party socket state timeout")), 2000);
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

function nextSocketClose(socket: WebSocket, label: string): Promise<CloseEvent> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label} close timeout`)), 2000);
    socket.addEventListener("close", event => {
      clearTimeout(timer);
      resolve(event);
    }, {once: true});
  });
}

function observeMatching(socket: WebSocket,
                         predicate: (value: Record<string, any>) => boolean) {
  let matched = false;
  const listener = (event: MessageEvent) => {
    const value = JSON.parse(String(event.data)) as Record<string, any>;
    if (predicate(value)) matched = true;
  };
  socket.addEventListener("message", listener);
  return {
    matched: () => matched,
    close: () => socket.removeEventListener("message", listener),
  };
}

async function admittedControlUnits(): Promise<number> {
  const bindings = env as unknown as Env;
  const day = new Date().toISOString().slice(0, 10);
  const response = await bindings.PARTY_BUDGETS.get(
    bindings.PARTY_BUDGETS.idFromName(day)).fetch("https://budget/status", {
      headers: {[INTERNAL_API_HEADER]: "1"},
    });
  const value = await response.json() as {
    admitted: {controlUnits: number};
  };
  return value.admitted.controlUnits;
}

describe("Party Worker local workerd adapter", () => {
  it("bootstraps a native host over one originless, versioned WSS upgrade", async () => {
    const response = await SELF.fetch("https://party.test/api/party/native-create", {
      headers: {upgrade: "websocket", "sec-websocket-protocol":
        `gb-native-host-v1, gb-control-v1, gb-key.${hostPublicKey}`},
    });
    expect(response.status).toBe(101);
    expect(response.headers.get("sec-websocket-protocol")).toBe("gb-control-v1");
    const socket = response.webSocket!;
    const messages = nextMessages(socket, 2);
    socket.accept();
    const received = await messages;
    const bootstrap = received[0]!;
    const state = received[1]!;
    expect(bootstrap).toMatchObject({type: "native_bootstrap",
      inviteGeneration: 1, fallbackCode: expect.stringMatching(/^\d{6}$/),
      controllerUrl: expect.stringMatching(/^https:\/\//)});
    expect(bootstrap.inviteExpiresInMs).toBeGreaterThan(0);
    expect(bootstrap.inviteExpiresInMs).toBeLessThanOrEqual(LIMITS.inviteTtlMs);
    expect(bootstrap.roomId).toMatch(/^[A-Za-z0-9_-]{22}$/);
    expect(bootstrap.hostCredential).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(state).toMatchObject({type: "room_state", phase: "open",
      transitionId: 1, controllers: []});
    expect(JSON.stringify(state)).not.toContain(bootstrap.hostCredential);
    expect(JSON.stringify(state)).not.toContain(bootstrap.fallbackCode);

    const reconnected = await SELF.fetch(
      `https://party.test/api/party/${bootstrap.roomId}/connect`, {headers: {
        upgrade: "websocket", "sec-websocket-protocol":
          `gb-native-host-v1, gb-control-v1, gb-host.${bootstrap.hostCredential}`,
      }});
    expect(reconnected.status).toBe(101);
    expect(reconnected.headers.get("sec-websocket-protocol")).toBe("gb-control-v1");
    reconnected.webSocket!.accept();
    reconnected.webSocket!.close(1000, "reconnect_contract_verified");

    const capability = new URL(bootstrap.controllerUrl).hash.slice(1);
    const pendingState = nextMessageMatching(socket, value =>
      value.type === "room_state" && value.transitionId === 2);
    const redeemed = await post("/api/controller/redeem", {
      capability, protocol: 1, name: "Native guest", controllerPublicKey,
    });
    expect(redeemed.status).toBe(201);
    const controller = await redeemed.json() as Record<string, string>;
    expect(await pendingState).toMatchObject({controllers: [{
      controllerId: controller.controllerId, phase: "pending",
    }]});
    const controlBeforeCommands = await admittedControlUnits();

    const invalidCommand = nextMessageMatching(socket, value =>
      value.type === "host_command_result");
    socket.send(JSON.stringify({type: "host_command", action: "approve",
      controllerId: controller.controllerId, seat: 2, diagnostics: true}));
    expect(await invalidCommand).toEqual({type: "host_command_result", ok: false,
      error: "invalid_command"});
    expect(await admittedControlUnits()).toBe(controlBeforeCommands);

    const approvedState = nextMessageMatching(socket, value =>
      value.type === "room_state" && value.transitionId === 3);
    socket.send(JSON.stringify({type: "host_command", action: "approve",
      controllerId: controller.controllerId, seat: 2}));
    expect(await approvedState).toMatchObject({controllers: [{phase: "leased",
      seat: 2, leaseGeneration: 1, connectionSequence: 1}]});

    const removedState = nextMessageMatching(socket, value =>
      value.type === "room_state" && value.transitionId === 4);
    socket.send(JSON.stringify({type: "host_command", action: "remove",
      controllerId: controller.controllerId}));
    expect(await removedState).toMatchObject({controllers: []});

    const rotatedState = nextMessageMatching(socket, value =>
      value.type === "room_state" && value.transitionId === 5 &&
      typeof value.controllerUrl === "string");
    socket.send(JSON.stringify({type: "host_command", action: "rotate",
      expectedInviteGeneration: 1}));
    const rotated = await rotatedState;
    expect(rotated).toMatchObject({inviteGeneration: 2,
      fallbackCode: expect.stringMatching(/^\d{6}$/),
      controllerUrl: expect.stringMatching(/^https:\/\//)});
    expect(rotated.controllerUrl).not.toBe(bootstrap.controllerUrl);

    const staleRotation = nextMessageMatching(socket, value =>
      value.type === "host_command_result");
    socket.send(JSON.stringify({type: "host_command", action: "rotate",
      expectedInviteGeneration: 1}));
    expect(await staleRotation).toEqual({type: "host_command_result", ok: false,
      error: "invalid_state"});

    const revokedState = nextMessageMatching(socket, value =>
      value.type === "room_state" && value.transitionId === 6);
    socket.send(JSON.stringify({type: "host_command", action: "revoke"}));
    expect(await revokedState).toMatchObject({inviteGeneration: 3});
    expect(await admittedControlUnits() - controlBeforeCommands).toBe(16);
    socket.close(1000, "test_complete");

    const browserAttempt = await SELF.fetch(
      "https://party.test/api/party/native-create", {headers: {
        origin: "https://attacker.invalid", upgrade: "websocket",
        "sec-websocket-protocol":
          `gb-native-host-v1, gb-control-v1, gb-key.${hostPublicKey}`,
      }});
    expect(browserAttempt.status).toBe(403);
    expect(await browserAttempt.json()).toEqual({error: "origin_forbidden"});

    const malformed = await SELF.fetch(
      "https://party.test/api/party/native-create", {headers: {
        upgrade: "websocket", "sec-websocket-protocol":
          "gb-native-host-v1, gb-control-v1, gb-key.short",
      }});
    expect(malformed.status).toBe(403);
  });

  it("rejects unknown Worker-to-object protocol versions at every boundary", async () => {
    const bindings = env as unknown as Env;
    const headers = {"content-type": "application/json",
      [INTERNAL_API_HEADER]: "future-unsupported"};
    const calls = [
      bindings.PARTY_BUDGETS.get(bindings.PARTY_BUDGETS.idFromName(
        "unknown-internal-budget")).fetch(
        "https://budget/admit?kind=pairing&units=1", {method: "POST", headers}),
      bindings.PARTY_ROOMS.get(bindings.PARTY_ROOMS.idFromName(
        "unknown-internal-room")).fetch(
        "https://room/initialize", {method: "POST", headers, body: "{}"}),
      bindings.PARTY_CODES.get(bindings.PARTY_CODES.idFromName(
        "unknown-internal-code")).fetch(
        "https://code/register", {method: "POST", headers, body: "{}"}),
      bindings.MATCH_ROOMS.get(bindings.MATCH_ROOMS.idFromName(
        "unknown-internal-match")).fetch(
        "https://match/initialize", {method: "POST", headers, body: "{}"}),
    ];
    for (const response of await Promise.all(calls)) {
      expect(response.status).toBe(426);
      expect(await response.json()).toEqual({error: "protocol_update_required"});
    }
  });

  it("hides the operator capacity snapshot when its secret is unprovisioned", async () => {
    const response = await SELF.fetch("https://party.test/api/ops/capacity", {
      headers: {origin},
    });
    expect(response.status).toBe(404);
    expect(response.headers.get("cache-control")).toBe("no-store");
    expect(await response.json()).toEqual({error: "not_found"});
  });

  it("migrates legacy raw controller codes to v2 digests on reconstruction", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    const room = await created.json() as Record<string, string>;
    expect(Number(room.inviteExpiresInMs)).toBeGreaterThan(0);
    expect(Number(room.inviteExpiresInMs)).toBeLessThanOrEqual(LIMITS.inviteTtlMs);
    const bindings = env as unknown as Env;
    const stub = bindings.PARTY_ROOMS.get(
      bindings.PARTY_ROOMS.idFromName(room.roomId!));
    await runInDurableObject(stub, async (_instance, state) => {
      const current = await state.storage.get<Record<string, unknown>>("room");
      const legacy: Record<string, unknown> = {...(current || {}), version: 1,
        fallbackCode: room.fallbackCode};
      delete legacy.fallbackCodeDigest;
      await state.storage.put("room", legacy);
    });
    await abortAllDurableObjects();
    const revoked = await post(`/api/party/${room.roomId}/revoke`, {},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(revoked.status).toBe(200);
    const resumedStub = bindings.PARTY_ROOMS.get(
      bindings.PARTY_ROOMS.idFromName(room.roomId!));
    const migrated = await runInDurableObject(resumedStub, async (_instance, state) =>
      state.storage.get<Record<string, unknown>>("room"));
    expect(migrated).toMatchObject({version: 2, fallbackCodeDigest: ""});
    expect(JSON.stringify(migrated)).not.toContain(room.fallbackCode!);
  });

  it("creates, redeems and host-approves without a cloud resource", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    expect(created.status).toBe(201);
    expect(created.headers.get("cache-control")).toBe("no-store");
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    expect(capability).toMatch(/^[A-Za-z0-9_-]{43}$/);

    const redeemed = await post("/api/controller/redeem", {
      capability, protocol: 1, name: "Test phone", controllerPublicKey,
    });
    expect(redeemed.status).toBe(201);
    const controller = await redeemed.json() as Record<string, string>;
    expect(controller.hostPublicKey).toBe(hostPublicKey);
    const approved = await post(
      `/api/party/${room.roomId}/approve`,
      {controllerId: controller.controllerId, seat: 2},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(await approved.json()).toMatchObject({ok: true, seat: 2,
      leaseGeneration: 1, connectionSequence: 1});

    const bindings = env as unknown as Env;
    const roomStub = bindings.PARTY_ROOMS.get(
      bindings.PARTY_ROOMS.idFromName(room.roomId!));
    const roomStorage = await runInDurableObject(roomStub, async (_instance, state) =>
      JSON.stringify(await state.storage.get("room")));
    const directoryStub = bindings.PARTY_CODES.get(
      bindings.PARTY_CODES.idFromName("v1"));
    const directoryKeys = await runInDurableObject(directoryStub,
      async (_instance, state) => [...(await state.storage.list()).keys()]);
    expect(roomStorage).not.toContain(room.fallbackCode!);
    expect(directoryKeys).not.toContain(room.fallbackCode!);

    const codeRedeemed = await post("/api/controller/code", {
      code: room.fallbackCode, protocol: 1, name: "Code phone", controllerPublicKey,
    });
    expect(codeRedeemed.status).toBe(201);
    expect(await codeRedeemed.json()).toMatchObject({roomId: room.roomId,
      protocol: 1});
  });

  it("serializes competing seat approvals without split ownership", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    const redeem = async (name: string, key: string) => {
      const response = await post("/api/controller/redeem", {
        capability, protocol: 1, name, controllerPublicKey: key.repeat(87),
      });
      expect(response.status).toBe(201);
      return response.json() as Promise<Record<string, string>>;
    };
    const [first, second] = await Promise.all([
      redeem("First phone", "A"), redeem("Second phone", "B"),
    ]);
    const approve = (controllerId: string) => post(
      `/api/party/${room.roomId}/approve`, {controllerId, seat: 2},
      {authorization: `Bearer ${room.hostCredential}`});
    const competing = await Promise.all([
      approve(first.controllerId!), approve(second.controllerId!),
    ]);
    expect(competing.map(response => response.status).sort()).toEqual([200, 409]);
    const bodies = await Promise.all(competing.map(response => response.json())) as
      Record<string, unknown>[];
    expect(bodies.filter(value => value.ok === true)).toHaveLength(1);
    expect(bodies.filter(value => value.error === "room_full")).toHaveLength(1);

    const bindings = env as unknown as Env;
    const roomStub = bindings.PARTY_ROOMS.get(
      bindings.PARTY_ROOMS.idFromName(room.roomId!));
    const stored = await runInDurableObject(roomStub, async (_instance, state) =>
      state.storage.get<Record<string, any>>("room"));
    expect(stored?.controllers.filter((value: Record<string, unknown>) =>
      value.phase === "leased" && value.seat === 2)).toHaveLength(1);
    expect(stored?.controllers.filter((value: Record<string, unknown>) =>
      value.phase === "pending")).toHaveLength(1);
  });

  it("fails closed for origins, oversize bodies and rotated invites", async () => {
    const forbidden = await SELF.fetch("https://party.test/api/party/create",
      {method: "POST", headers: {origin: "https://attacker.invalid"}});
    expect(forbidden.status).toBe(403);

    const oversized = await SELF.fetch("https://party.test/api/controller/redeem", {
      method: "POST", headers: {origin, "content-type": "application/json",
        "content-length": String(1024 * 1024)}, body: "{}",
    });
    expect(oversized.status).toBe(413);
    expect(oversized.headers.get("cache-control")).toBe("no-store");
    expect(await oversized.json()).toEqual({error: "request_too_large"});

    const created = await post("/api/party/create", {hostPublicKey});
    const room = await created.json() as Record<string, string>;
    const originalCapability = new URL(room.controllerUrl!).hash.slice(1);
    const originalCode = room.fallbackCode;
    const rotated = await post(`/api/party/${room.roomId}/rotate`,
      {expectedInviteGeneration: 1},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(rotated.status).toBe(200);
    const next = await rotated.json() as Record<string, string | number>;
    expect(next.inviteGeneration).toBe(2);
    expect(next.inviteExpiresInMs).toBeGreaterThan(0);
    expect(next.inviteExpiresInMs).toBeLessThanOrEqual(LIMITS.inviteTtlMs);
    expect(next.controllerUrl).not.toBe(room.controllerUrl);
    expect(next.fallbackCode).toMatch(/^\d{6}$/);
    const replay = await post("/api/controller/redeem",
      {capability: originalCapability, protocol: 1, controllerPublicKey});
    expect(replay.status).toBe(409);
    expect(await replay.json()).toEqual({error: "invite_rotated"});
    const codeReplay = await post("/api/controller/code",
      {code: originalCode, protocol: 1, controllerPublicKey});
    expect(codeReplay.status).toBe(409);
    expect(await codeReplay.json()).toEqual({error: "invite_rotated"});

    const socketResponse = await SELF.fetch(
      `https://party.test/api/party/${room.roomId}/connect`, {headers: {
        origin, upgrade: "websocket",
        "sec-websocket-protocol": `gb-control-v1, gb-host.${room.hostCredential}`,
      }});
    expect(socketResponse.status).toBe(101);
    const socket = socketResponse.webSocket!;
    socket.accept();
    const close = nextSocketClose(socket, "oversized UTF-8 signaling");
    socket.send("é".repeat(32_769));
    expect(await close).toMatchObject({code: 4009, reason: "message_too_large"});
  });

  it("rejects unknown public Phone Party fields without mutating authority", async () => {
    const unknownCreate = await post("/api/party/create",
      {hostPublicKey, diagnostics: true});
    expect(unknownCreate.status).toBe(400);
    expect(await unknownCreate.json()).toEqual({error: "invalid_host_key"});

    const created = await post("/api/party/create", {hostPublicKey});
    expect(created.status).toBe(201);
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    const unknownRedeem = await post("/api/controller/redeem", {
      capability, protocol: 1, controllerPublicKey, diagnostics: true,
    });
    expect(unknownRedeem.status).toBe(400);
    expect(await unknownRedeem.json()).toEqual({error: "invalid_invite"});

    const unknownRevoke = await post(`/api/party/${room.roomId}/revoke`,
      {diagnostics: true},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(unknownRevoke.status).toBe(400);
    expect(await unknownRevoke.json()).toEqual({error: "invalid_request"});

    const stillRedeemable = await post("/api/controller/redeem",
      {capability, protocol: 1, controllerPublicKey});
    expect(stillRedeemable.status).toBe(201);
  });

  it("closes ambiguous or non-object Phone Party signaling before relay", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    expect(created.status).toBe(201);
    const room = await created.json() as Record<string, string>;
    const connectHost = async () => {
      const response = await SELF.fetch(
        `https://party.test/api/party/${room.roomId}/connect`, {headers: {
          origin, upgrade: "websocket",
          "sec-websocket-protocol": `gb-control-v1, gb-host.${room.hostCredential}`,
        }});
      expect(response.status).toBe(101);
      const socket = response.webSocket!;
      const initial = nextMessageMatching(socket, value => value.type === "room_state");
      socket.accept();
      await initial;
      return socket;
    };

    const extraField = await connectHost();
    const extraClose = nextSocketClose(extraField, "unknown signaling field");
    extraField.send(JSON.stringify({type: "webrtc_offer", to: "A".repeat(22),
      peerGeneration: 1, sdp: {type: "offer", sdp: "v=0\r\n"},
      diagnostics: true}));
    expect(await extraClose).toMatchObject({code: 4003,
      reason: "invalid_signaling"});

    const nonObject = await connectHost();
    const nonObjectClose = nextSocketClose(nonObject, "non-object signaling");
    nonObject.send("null");
    expect(await nonObjectClose).toMatchObject({code: 4003,
      reason: "invalid_signaling"});
  });

  it("delivers host WebRTC signaling only to its addressed controller", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    expect(created.status).toBe(201);
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    const redeem = async (name: string, key: string) => {
      const response = await post("/api/controller/redeem", {
        capability, protocol: 1, name, controllerPublicKey: key.repeat(87),
      });
      expect(response.status).toBe(201);
      return response.json() as Promise<Record<string, string>>;
    };
    const first = await redeem("First phone", "A");
    const second = await redeem("Second phone", "B");
    expect((await post(`/api/party/${room.roomId}/approve`,
      {controllerId: first.controllerId, seat: 1},
      {authorization: `Bearer ${room.hostCredential}`})).status).toBe(200);
    expect((await post(`/api/party/${room.roomId}/approve`,
      {controllerId: second.controllerId, seat: 2},
      {authorization: `Bearer ${room.hostCredential}`})).status).toBe(200);

    const connectSocket = async (role: "host" | "controller", credential: string,
                                 initialType: string) => {
      const response = await SELF.fetch(
        `https://party.test/api/party/${room.roomId}/connect`, {headers: {
          origin, upgrade: "websocket",
          "sec-websocket-protocol":
            `gb-control-v1, gb-${role}.${credential}`,
        }});
      expect(response.status).toBe(101);
      const socket = response.webSocket!;
      const initial = nextMessageMatching(socket, value => value.type === initialType);
      socket.accept();
      await initial;
      return socket;
    };
    const host = await connectSocket("host", room.hostCredential!, "room_state");
    const firstSocket = await connectSocket(
      "controller", first.credential!, "controller_state");
    const secondSocket = await connectSocket(
      "controller", second.credential!, "controller_state");
    const hello = nextMessageMatching(host, value =>
      value.type === "controller_hello");
    firstSocket.send(JSON.stringify({type: "controller_hello",
      controllerId: second.controllerId}));
    expect(await hello).toEqual({type: "controller_hello",
      controllerId: first.controllerId});
    const offer = nextMessageMatching(firstSocket, value =>
      value.type === "webrtc_offer" && value.to === first.controllerId);
    const leak = observeMatching(secondSocket, value => value.type === "webrtc_offer");
    host.send(JSON.stringify({type: "webrtc_offer", to: first.controllerId,
      peerGeneration: 1, sdp: {type: "offer", sdp: "v=0\r\n"}}));
    expect(await offer).toMatchObject({to: first.controllerId,
      peerGeneration: 1, sdp: {type: "offer"}});
    await new Promise(resolve => setTimeout(resolve, 25));
    expect(leak.matched()).toBe(false);
    leak.close();

    const removedControllerState = nextMessageMatching(firstSocket, value =>
      value.type === "controller_state" && value.phase === "closed");
    const removedHostState = nextMessageMatching(host, value =>
      value.type === "room_state" &&
      value.controllers.every((item: Record<string, unknown>) =>
        item.controllerId !== first.controllerId));
    const removedSocketClose = nextSocketClose(firstSocket, "removed controller");
    const removed = await post(`/api/party/${room.roomId}/remove`,
      {controllerId: first.controllerId},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(removed.status).toBe(200);
    const removedResult = await removed.json() as Record<string, unknown>;
    expect(removedResult).toMatchObject({ok: true,
      controllerId: first.controllerId});
    expect(await removedControllerState).toEqual({type: "controller_state",
      transitionId: removedResult.transitionId, phase: "closed", seat: null,
      leaseGeneration: 1, connectionSequence: 2});
    expect(await removedHostState).toMatchObject({controllers: [{
      controllerId: second.controllerId, seat: 2,
    }]});
    expect(await removedSocketClose).toMatchObject({code: 4000,
      reason: "seat_reclaimed"});

    for (const socket of [host, secondSocket]) {
      socket.close(1000, "targeting_contract_verified");
    }
  });

  it("revokes a dismissed QR while preserving the room", async () => {
    const created = await post("/api/party/create", {hostPublicKey});
    const room = await created.json() as Record<string, string>;
    const capability = new URL(room.controllerUrl!).hash.slice(1);
    const revoked = await post(`/api/party/${room.roomId}/revoke`, {},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(revoked.status).toBe(200);
    expect(await revoked.clone().json()).toEqual({ok: true, transitionId: 2,
      inviteGeneration: 2});
    const replay = await post("/api/controller/redeem",
      {capability, protocol: 1, controllerPublicKey});
    expect(replay.status).toBe(409);
    expect(await replay.json()).toEqual({error: "invite_expired"});
    const lateRotate = await post(`/api/party/${room.roomId}/rotate`,
      {expectedInviteGeneration: 1},
      {authorization: `Bearer ${room.hostCredential}`});
    expect(lateRotate.status).toBe(409);
    expect(await lateRotate.json()).toEqual({error: "invalid_state"});
  });

  it("rate-limits fallback guessing per pseudonymous requester", async () => {
    for (let attempt = 0; attempt < 12; attempt++) {
      const response = await post("/api/controller/code",
        {code: "999999", protocol: 1, controllerPublicKey},
        {"cf-connecting-ip": "192.0.2.44"});
      expect(response.status).toBe(404);
    }
    const limited = await post("/api/controller/code",
      {code: "999999", protocol: 1, controllerPublicKey},
      {"cf-connecting-ip": "192.0.2.44"});
    expect(limited.status).toBe(429);
    expect(await limited.json()).toEqual({error: "rate_limited"});
  });
});
