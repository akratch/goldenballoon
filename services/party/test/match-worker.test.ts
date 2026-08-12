import {env} from "cloudflare:workers";
import {SELF, abortAllDurableObjects, evictDurableObject,
  runDurableObjectAlarm, runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import type {Env} from "../src/types";

const origin = "https://party.example.invalid";
const compatibility = {protocolVersion: 1,
  buildId: Array.from({length: 16}, (_, i) => i + 1),
  gameplayDigest: Array.from({length: 32}, (_, i) => 128 + i),
  romRevision: 1, cadenceHz: 30};

async function post(path: string, value?: unknown, credential?: string) {
  const headers = new Headers({origin});
  if (value !== undefined) headers.set("content-type", "application/json");
  if (credential) headers.set("authorization", `Bearer ${credential}`);
  const init: RequestInit = {method: "POST", headers};
  if (value !== undefined) init.body = JSON.stringify(value);
  return SELF.fetch(`https://party.test${path}`, init);
}

async function create() {
  const response = await post("/api/match/create", {compatibility, seatCount: 1});
  expect(response.status).toBe(201);
  return response.json() as Promise<Record<string, any>>;
}

function command(revision: number, commandId: string, type: string,
                 value = 0, targetEndpointId = "0") {
  return {protocolVersion: 1, expectedRevision: revision, commandId,
    type, value, targetEndpointId};
}

function nextMessage(socket: WebSocket, label: string): Promise<Record<string, any>> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`match socket ${label} timeout`)), 2000);
    socket.addEventListener("message", event => {
      clearTimeout(timer);
      resolve(JSON.parse(String(event.data)) as Record<string, any>);
    }, {once: true});
  });
}

function nextClose(socket: WebSocket, label: string): Promise<CloseEvent> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`match socket ${label} close timeout`)),
      2000);
    socket.addEventListener("close", event => {
      clearTimeout(timer);
      resolve(event);
    }, {once: true});
  });
}

describe("MatchRoom local Durable Object adapter", () => {
  it("joins by an isolated rate-limited code without persisting the raw code", async () => {
    const host = await create();
    expect(new URL(host.inviteUrl).pathname).toBe("/room/");
    expect(host.fallbackCode).toMatch(/^\d{6}$/);
    const joined = await post("/api/match/code", {code: host.fallbackCode,
      compatibility, seatCount: 1}, undefined);
    expect(joined.status).toBe(201);
    expect(await joined.json()).toMatchObject({roomId: host.roomId,
      lobby: {revision: 2}});

    const bindings = env as unknown as Env;
    const roomStub = bindings.MATCH_ROOMS.get(
      bindings.MATCH_ROOMS.idFromName(host.roomId));
    const roomStorage = await runInDurableObject(roomStub, async (_instance, state) =>
      JSON.stringify(await state.storage.get("match")));
    const directoryStub = bindings.PARTY_CODES.get(
      bindings.PARTY_CODES.idFromName("match-v1"));
    const directoryKeys = await runInDurableObject(directoryStub,
      async (_instance, state) => [...(await state.storage.list()).keys()]);
    expect(roomStorage).not.toContain(host.fallbackCode);
    expect(directoryKeys).not.toContain(host.fallbackCode);
    expect(directoryKeys.some(key => key.startsWith("code:") && key.length === 48))
      .toBe(true);

    const wrongDirectory = await post("/api/controller/code", {
      code: host.fallbackCode, protocol: 1,
      controllerPublicKey: "C".repeat(87)});
    expect(wrongDirectory.status).toBe(404);
  });

  it("resumes a hibernated authenticated state socket after object restart", async () => {
    const host = await create();
    const response = await SELF.fetch(
      `https://party.test/api/match/${host.roomId}/connect`, {headers: {
        origin, upgrade: "websocket",
        "sec-websocket-protocol": `gb-match-v1, gb-match.${host.credential}`,
      }});
    expect(response.status).toBe(101);
    const socket = response.webSocket!;
    const initialMessage = nextMessage(socket, "initial message");
    socket.accept();
    expect(await initialMessage).toMatchObject({type: "match_state",
      lobby: {revision: host.lobby.revision}});
    const bindings = env as unknown as Env;
    const stub = bindings.MATCH_ROOMS.get(bindings.MATCH_ROOMS.idFromName(host.roomId));
    await evictDurableObject(stub, {webSockets: "hibernate"});
    const resumedMessage = nextMessage(socket, "resumed message");
    const changed = await post(`/api/match/${host.roomId}/command`,
      command(host.lobby.revision, "1", "set_character", 0, "0"),
      host.credential);
    expect(changed.status).toBe(200);
    expect(await resumedMessage).toMatchObject({type: "match_state",
      lobby: {revision: host.lobby.revision + 1}});
    socket.close(1000, "test_complete");
  }, 10_000);

  it("bounds forbidden text and binary socket messages before closing", async () => {
    const host = await create();
    const connect = async () => {
      const response = await SELF.fetch(
        `https://party.test/api/match/${host.roomId}/connect`, {headers: {
          origin, upgrade: "websocket",
          "sec-websocket-protocol": `gb-match-v1, gb-match.${host.credential}`,
        }});
      expect(response.status).toBe(101);
      const socket = response.webSocket!;
      const initial = nextMessage(socket, "size-bound initial state");
      socket.accept();
      await initial;
      return socket;
    };

    const ordinary = await connect();
    const ordinaryClose = nextClose(ordinary, "ordinary forbidden message");
    ordinary.send("command");
    expect(await ordinaryClose).toMatchObject({code: 4003,
      reason: "commands_use_authenticated_http"});

    for (const payload of ["é".repeat(2049), new Uint8Array(4097).buffer]) {
      const socket = await connect();
      const closed = nextClose(socket, typeof payload === "string" ? "utf8" : "binary");
      socket.send(payload);
      expect(await closed).toMatchObject({code: 4009, reason: "message_too_large"});
    }
  });

  it("runs the complete two-endpoint room barrier and rematch through HTTP", async () => {
    const host = await create();
    const capability = new URL(host.inviteUrl).hash.slice("#match=".length);
    const joined = await post("/api/match/join",
      {capability, compatibility, seatCount: 1});
    const guest = await joined.json() as Record<string, any>;
    let revision = guest.lobby.revision as number;
    const restartAt = async (phase: string) => {
      await abortAllDurableObjects();
      const state = await post(`/api/match/${host.roomId}/state`, {}, host.credential);
      expect(state.status).toBe(200);
      expect(await state.json()).toMatchObject({lobby: {phase, revision}});
    };
    const send = async (credential: string, id: number, type: string,
                        value = 0, target = "0") => {
      const response = await post(`/api/match/${host.roomId}/command`,
        command(revision, String(id), type, value, target), credential);
      const body = await response.json() as Record<string, any>;
      if (response.ok && !body.duplicate) revision = body.revision;
      return {response, body};
    };
    await restartAt("lobby");
    expect((await send(host.credential, 1, "set_character", 0, "0")).response.ok).toBe(true);
    expect((await send(host.credential, 2, "set_vehicle", 0, "0")).response.ok).toBe(true);
    expect((await send(guest.credential, 2, "set_character", 1, "1")).response.ok).toBe(true);
    expect((await send(guest.credential, 3, "set_vehicle", 0, "1")).response.ok).toBe(true);
    expect((await send(host.credential, 3, "set_vote", 5, "0")).response.ok).toBe(true);
    expect((await send(guest.credential, 4, "set_vote", 3, "1")).response.ok).toBe(true);
    expect((await send(host.credential, 4, "set_ready", 1)).response.ok).toBe(true);
    expect((await send(guest.credential, 5, "set_ready", 1)).response.ok).toBe(true);
    const unauthorized = await send(guest.credential, 6, "begin_loading", 7);
    expect(unauthorized.response.status).toBe(409);
    expect(unauthorized.body).toMatchObject({error: "unauthorized", revision});
    expect((await send(host.credential, 5, "begin_loading", 7)).body)
      .toMatchObject({accepted: true, matchEpoch: 1});
    await restartAt("loading");
    expect((await send(host.credential, 6, "ack_loaded")).response.ok).toBe(true);
    expect((await send(guest.credential, 6, "ack_loaded")).response.ok).toBe(true);
    expect((await send(host.credential, 7, "begin_race")).response.ok).toBe(true);
    await restartAt("racing");
    expect((await send(host.credential, 8, "publish_results")).response.ok).toBe(true);
    await restartAt("results");
    expect((await send(host.credential, 9, "rematch")).response.ok).toBe(true);
    const state = await post(`/api/match/${host.roomId}/state`, {}, guest.credential);
    expect(await state.json()).toMatchObject({lobby: {phase: "lobby", matchEpoch: 1,
      selectedTrack: null, selectedVehicleMask: 0}});
    expect((await send(host.credential, 10, "close")).response.ok).toBe(true);
    await restartAt("closed");
  });

  it("creates, joins, authenticates, survives eviction and rejects split brain", async () => {
    const host = await create();
    const capability = new URL(host.inviteUrl).hash.slice("#match=".length);
    expect(capability).toMatch(/^[A-Za-z0-9_-]{43}$/);
    const joinedResponse = await post("/api/match/join",
      {capability, compatibility, seatCount: 1});
    expect(joinedResponse.status).toBe(201);
    const guest = await joinedResponse.json() as Record<string, any>;
    expect(guest.lobby).toMatchObject({revision: 2, phase: "lobby"});
    expect(guest.credential).toMatch(/^[A-Za-z0-9_-]{43}$/);

    const unauthorized = await post(`/api/match/${host.roomId}/state`, {});
    expect(unauthorized.status).toBe(401);
    const state = await post(`/api/match/${host.roomId}/state`, {}, host.credential);
    expect(state.status).toBe(200);
    const before = await state.json() as Record<string, any>;
    expect(JSON.stringify(before)).not.toContain(host.credential);
    expect(JSON.stringify(before)).not.toContain(guest.credential);
    expect(before.lobby).not.toHaveProperty("receipts");
    expect(before.lobby).not.toHaveProperty("nextReceipt");
    expect(before.lobby.members[0]).not.toHaveProperty("lastCommandId");
    expect(before.lobby.members[0]).not.toHaveProperty("lastCommandFingerprint");

    const competing = await Promise.all([
      post(`/api/match/${host.roomId}/command`,
        command(before.lobby.revision, "1", "set_character", 0, "0"), host.credential),
      post(`/api/match/${host.roomId}/command`,
        command(before.lobby.revision, "2", "set_vehicle", 0, "0"), host.credential),
    ]);
    expect(competing.map(item => item.status).sort()).toEqual([200, 412]);
    await abortAllDurableObjects();
    const resumed = await post(`/api/match/${host.roomId}/state`, {}, host.credential);
    expect(resumed.status).toBe(200);
    expect((await resumed.json() as Record<string, any>).lobby.revision)
      .toBe(before.lobby.revision + 1);
  }, 15_000);

  it("rotates host invites atomically and rejects stale links and codes", async () => {
    const host = await create();
    const oldCapability = new URL(host.inviteUrl).hash.slice("#match=".length);
    const rotatedResponse = await post(`/api/match/${host.roomId}/rotate`,
      {expectedInviteGeneration: 1}, host.credential);
    expect(rotatedResponse.status).toBe(200);
    const rotated = await rotatedResponse.json() as Record<string, any>;
    expect(rotated).toMatchObject({inviteGeneration: 2});
    expect(rotated.inviteUrl).not.toBe(host.inviteUrl);
    expect(rotated.fallbackCode).toMatch(/^\d{6}$/);

    const staleLink = await post("/api/match/join",
      {capability: oldCapability, compatibility, seatCount: 1});
    expect(staleLink.status).toBe(409);
    expect(await staleLink.json()).toEqual({error: "invalid_invite"});
    const staleCode = await post("/api/match/code",
      {code: host.fallbackCode, compatibility, seatCount: 1});
    expect(staleCode.status).toBe(409);
    expect(await staleCode.json()).toEqual({error: "invalid_invite"});

    const newCapability = new URL(rotated.inviteUrl).hash.slice("#match=".length);
    const guestResponse = await post("/api/match/join",
      {capability: newCapability, compatibility, seatCount: 1});
    expect(guestResponse.status).toBe(201);
    const guest = await guestResponse.json() as Record<string, any>;
    const guestRotation = await post(`/api/match/${host.roomId}/rotate`,
      {expectedInviteGeneration: 2}, guest.credential);
    expect(guestRotation.status).toBe(403);
    expect(await guestRotation.json()).toEqual({error: "unauthorized"});
    expect((await post("/api/match/code",
      {code: rotated.fallbackCode, compatibility, seatCount: 1})).status).toBe(201);

    const staleRotation = await post(`/api/match/${host.roomId}/rotate`,
      {expectedInviteGeneration: 1}, host.credential);
    expect(staleRotation.status).toBe(409);
    expect(await staleRotation.json()).toEqual({error: "invalid_state"});
  });

  it("makes exact retries idempotent and conflicting replays atomic", async () => {
    const host = await create();
    const initial = host.lobby.revision as number;
    const body = command(initial, "1", "set_character", 0, "0");
    const first = await post(`/api/match/${host.roomId}/command`, body, host.credential);
    expect(first.status).toBe(200);
    const accepted = await first.json() as Record<string, any>;
    const retry = await post(`/api/match/${host.roomId}/command`, body, host.credential);
    expect(retry.status).toBe(200);
    expect(await retry.json()).toMatchObject({accepted: true, duplicate: true,
      revision: accepted.revision});
    const conflict = await post(`/api/match/${host.roomId}/command`,
      {...body, value: 2}, host.credential);
    expect(conflict.status).toBe(409);
    expect(await conflict.json()).toMatchObject({error: "command_conflict",
      revision: accepted.revision});
    const malleable = await post(`/api/match/${host.roomId}/command`,
      {...command(accepted.revision, "2", "set_vehicle", 0, "0"), compatibility},
      host.credential);
    expect(malleable.status).toBe(400);
    expect(await malleable.json()).toEqual({error: "invalid_command"});
  });

  it("fails closed for wrong compatibility, oversize commands and expiry", async () => {
    const host = await create();
    const capability = new URL(host.inviteUrl).hash.slice("#match=".length);
    const incompatible = structuredClone(compatibility);
    incompatible.gameplayDigest[0] = incompatible.gameplayDigest[0]! ^ 1;
    const rejected = await post("/api/match/join",
      {capability, compatibility: incompatible, seatCount: 1});
    expect(rejected.status).toBe(409);
    expect(await rejected.json()).toEqual({error: "incompatible"});

    const oversized = await SELF.fetch(
      `https://party.test/api/match/${host.roomId}/command`, {
        method: "POST", headers: {origin, authorization: `Bearer ${host.credential}`,
          "content-type": "application/json"}, body: "x".repeat(5000)});
    expect(oversized.status).toBe(413);

    const bindings = env as unknown as Env;
    const stub = bindings.MATCH_ROOMS.get(bindings.MATCH_ROOMS.idFromName(host.roomId));
    await runInDurableObject(stub, async (_instance, state) => {
      const record = await state.storage.get<Record<string, any>>("match");
      record!.lobby.revision = 4096;
      await state.storage.put("match", record);
    });
    const exhausted = await post(`/api/match/${host.roomId}/command`,
      command(4096, "1", "set_character", 0, "0"), host.credential);
    expect(exhausted.status).toBe(503);
    expect(await exhausted.json()).toEqual({error: "service_budget_safe"});
    expect(await runDurableObjectAlarm(stub)).toBe(true);
    const expired = await post(`/api/match/${host.roomId}/state`, {}, host.credential);
    expect(expired.status).toBe(404);
  });

});
