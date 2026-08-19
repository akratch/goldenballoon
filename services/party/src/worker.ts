import {PartyBudget} from "./party-budget";
import type {BudgetOperation} from "./party-budget";
import {PartyRoom} from "./party-room";
import {PartyCodeDirectory} from "./party-code-directory";
import {MatchRoom} from "./match/match-room";
import {MATCH_LIMITS, type MatchCompatibilityV1,
  inviteRemainingMs, validCompatibility,
  validMatchCommandRequest} from "./match/protocol";
import {allowedOrigin, base64Url, boundCredential, constantTimeEqual, digest,
  fallbackCode, fromBase64Url, json, normalizeName, readJson,
  randomToken, validBoundCredential, validPartyOrigin} from "./security";
import {LIMITS, partyInviteRemainingMs, type Env} from "./types";
import {internalRequest} from "./internal-api";

export {MatchRoom, PartyBudget, PartyCodeDirectory, PartyRoom};

function roomName(bytes: Uint8Array): string { return base64Url(bytes.slice(0, 16)); }

function numericId(bytes: Uint8Array): string {
  let value = 0n;
  for (const byte of bytes.slice(0, 8)) value = (value << 8n) | BigInt(byte);
  return (value || 1n).toString();
}

function validPublicKey(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{87}$/.test(value);
}

function exactKeys(value: Record<string, unknown>, expected: readonly string[]): boolean {
  const keys = Object.keys(value).sort();
  const wanted = [...expected].sort();
  return keys.length === wanted.length &&
    keys.every((key, index) => key === wanted[index]);
}

function exactKeysWithOptionalName(value: Record<string, unknown>,
                                   required: readonly string[]): boolean {
  return exactKeys(value, required) || exactKeys(value, [...required, "name"]);
}

/**
 * Charge the day's admission reserve for one external operation.
 *
 * Ordering contract, enforced by every call site below: a route charges only
 * after the request has proven it is well formed and — where the route has one
 * — has presented a valid bound credential. An unparseable, over-size,
 * unknown-field or unauthenticated request must cost zero units, so a flood of
 * empty POSTs can no longer spend the global reserve and fail the service
 * closed until UTC midnight.
 *
 * The charge is deliberately NOT refunded when an already-validated,
 * already-authenticated request is then refused for capacity or state reasons
 * (room full, stale invite generation, expired room, transition cap). Those
 * refusals are decided by the room's authoritative state, which means the
 * request has already consumed the Durable Object fanout it is being billed
 * for. Refunding them would also hand an attacker holding one valid credential
 * an unlimited free-work oracle: every refused request would be free, and
 * refusal is the cheapest state to drive a room into. Charging at the last
 * point where refusal is still free — the end of shape and credential
 * validation — is the conservative anti-abuse boundary.
 *
 * Create operations additionally carry the requester's pseudonymous source
 * digest (I-2): the budget object holds each address to a bounded daily
 * create allowance so one distributed abuser cannot drain the shared global
 * reserve and deny pairing to everyone until UTC midnight. A throttled
 * create is refused before anything is charged.
 */
async function budget(env: Env, kind: "pairing" | "control",
                      units: number, operation: BudgetOperation,
                      sourceDigest = ""): Promise<Response> {
  const day = new Date().toISOString().slice(0, 10);
  const id = env.PARTY_BUDGETS.idFromName(day);
  const response = await env.PARTY_BUDGETS.get(id).fetch(
    `https://budget/admit?kind=${kind}&units=${units}&operation=${operation}` +
    (sourceDigest ? `&source=${sourceDigest}` : ""),
    internalRequest({method: "POST"}));
  // The Budget object carries internal reserve accounting. External callers
  // receive two stable refusal enums — the per-address create throttle and
  // the global reserve — never its implementation fields or a
  // provider-shaped response that the launcher would have to understand.
  if (response.status === 429) return json({error: "create_rate_limited"}, 429);
  return response.ok ? response : json({error: "service_budget_safe"}, 503);
}

/** One pseudonymous daily create bucket per connecting address. Reuses the
 * short-code directory's requester idiom (party-code-directory.ts): the raw
 * address is hashed and never stored or forwarded, and a missing header
 * (local development and tests) collapses to one shared bucket, which fails
 * safe — shared and throttled — rather than open. */
function createSource(request: Request, env: Env): Promise<string> {
  return digest(env, "create-source",
    request.headers.get("cf-connecting-ip") || "local-test");
}

async function operationsStatus(request: Request, env: Env,
                                view: "status" | "health"): Promise<Response> {
  const token = env.OPS_READ_TOKEN || "";
  if (token.length < 32) return json({error: "not_found"}, 404);
  const bearer = request.headers.get("authorization") || "";
  const supplied = bearer.startsWith("Bearer ") ? bearer.slice(7) : "";
  if (!constantTimeEqual(supplied, token)) return json({error: "unauthorized"}, 401);
  const day = new Date().toISOString().slice(0, 10);
  const id = env.PARTY_BUDGETS.idFromName(day);
  const response = await env.PARTY_BUDGETS.get(id).fetch(`https://budget/${view}`,
    internalRequest());
  if (!response.ok) return json({error: "service_unavailable"}, 503);
  const snapshot = await response.json() as Record<string, unknown>;
  return json({...snapshot, day});
}

function roomStub(env: Env, name: string): DurableObjectStub<PartyRoom> {
  return env.PARTY_ROOMS.get(env.PARTY_ROOMS.idFromName(name));
}

function codeStub(env: Env, directory = "v1"): DurableObjectStub<PartyCodeDirectory> {
  return env.PARTY_CODES.get(env.PARTY_CODES.idFromName(directory));
}

function matchStub(env: Env, name: string): DurableObjectStub<MatchRoom> {
  return env.MATCH_ROOMS.get(env.MATCH_ROOMS.idFromName(name));
}

function validMatchSeatRequest(body: Record<string, unknown>): boolean {
  return validCompatibility(body.compatibility) &&
    Number.isInteger(body.seatCount) && Number(body.seatCount) >= 1 &&
    Number(body.seatCount) <= MATCH_LIMITS.maxSeatsPerEndpoint;
}

async function createMatch(request: Request, env: Env): Promise<Response> {
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeys(body, ["compatibility", "seatCount"]) ||
      !validMatchSeatRequest(body)) {
    return json({error: "invalid_match"}, 400);
  }
  /* Budget + eight hashed-code collision attempts + room initialize, charged
   * once the body is known to be a well-formed create request. */
  const admission = await budget(env, "pairing", 10, "matchCreate",
    await createSource(request, env));
  if (!admission.ok) return admission;
  const roomBytes = crypto.getRandomValues(new Uint8Array(16));
  const inviteBytes = new Uint8Array(32);
  inviteBytes.set(roomBytes);
  inviteBytes.set(crypto.getRandomValues(new Uint8Array(16)), 16);
  const capability = base64Url(inviteBytes);
  const roomId = roomName(roomBytes);
  const endpointId = numericId(crypto.getRandomValues(new Uint8Array(8)));
  const credential = await boundCredential(env, "match-endpoint", roomId);
  let matchCode = "";
  for (let attempt = 0; attempt < 8; attempt++) {
    const candidate = fallbackCode();
    if (await registerCode(env, candidate, roomId, "match-v1", "match-code",
      MATCH_LIMITS.inviteTtlMs)) { matchCode = candidate; break; }
  }
  if (!matchCode) return json({error: "room_create_failed"}, 503);
  const initialized = await matchStub(env, roomId).fetch("https://match/initialize",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify({roomNumericId: numericId(roomBytes), leaderEndpointId: endpointId,
      leaderCredentialDigest: await digest(env, "match-endpoint", credential),
      inviteDigest: await digest(env, "match-invite", capability),
      fallbackCodeDigest: await digest(env, "match-code", matchCode),
      compatibility: body.compatibility as MatchCompatibilityV1,
      leaderSeats: Number(body.seatCount), now: Date.now()}),
  }));
  if (!initialized.ok) return json({error: "room_create_failed"}, 503);
  const state = await initialized.json() as Record<string, unknown>;
  const inviteExpiresInMs = inviteRemainingMs(state.inviteExpiresAt, Date.now());
  if (inviteExpiresInMs === null) {
    return json({error: "room_create_failed"}, 503);
  }
  return json({...state, roomId, endpointId, credential, fallbackCode: matchCode,
    inviteExpiresInMs,
    inviteUrl: `${env.PARTY_ORIGIN}/room/#match=${capability}`}, 201);
}

async function joinMatchToRoom(body: Record<string, unknown>, env: Env,
                               roomId: string, proof: {
                                 inviteDigest?: string;
                                 fallbackCodeDigest?: string;
                               }): Promise<Response> {
  /* Both callers validate this before charging budget; kept as the last
   * defensive gate in front of the room object. */
  if (!validMatchSeatRequest(body)) return json({error: "invalid_invite"}, 400);
  const endpointId = numericId(crypto.getRandomValues(new Uint8Array(8)));
  const credential = await boundCredential(env, "match-endpoint", roomId);
  const response = await matchStub(env, roomId).fetch("https://match/join",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify({inviteDigest: proof.inviteDigest || "",
      fallbackCodeDigest: proof.fallbackCodeDigest || "",
      endpointId, credentialDigest: await digest(env, "match-endpoint", credential),
      compatibility: body.compatibility, seatCount: Number(body.seatCount),
      commandId: "1", now: Date.now()}),
  }));
  if (!response.ok) return response;
  const state = await response.json() as Record<string, unknown>;
  return json({...state, roomId, endpointId, credential}, 201);
}

async function joinMatch(request: Request, env: Env): Promise<Response> {
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeys(body, ["capability", "compatibility", "seatCount"]) ||
      typeof body.capability !== "string") {
    return json({error: "invalid_invite"}, 400);
  }
  const decoded = fromBase64Url(body.capability);
  if (!decoded || decoded.byteLength !== 32 || !validMatchSeatRequest(body)) {
    return json({error: "invalid_invite"}, 400);
  }
  const admission = await budget(env, "pairing", 2, "matchLinkJoin");
  if (!admission.ok) return admission;
  return joinMatchToRoom(body, env, roomName(decoded),
    {inviteDigest: await digest(env, "match-invite", body.capability)});
}

async function joinMatchCode(request: Request, env: Env): Promise<Response> {
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeys(body, ["code", "compatibility", "seatCount"])) {
    return json({error: "invalid_code"}, 400);
  }
  const code = typeof body.code === "string" ? body.code.replace(/\s/g, "") : "";
  if (!/^\d{6}$/.test(code)) return json({error: "invalid_code"}, 400);
  if (!validMatchSeatRequest(body)) return json({error: "invalid_invite"}, 400);
  const admission = await budget(env, "pairing", 3, "matchCodeJoin");
  if (!admission.ok) return admission;
  const codeDigest = await digest(env, "match-code", code);
  const resolved = await codeStub(env, "match-v1").fetch("https://code/resolve",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify({codeDigest,
      requesterDigest: await digest(env, "match-requester",
        request.headers.get("cf-connecting-ip") || "local-test")}),
  }));
  if (resolved.status === 429) return json({error: "rate_limited"}, 429);
  if (!resolved.ok) return json({error: "invalid_code"}, 404);
  const value = await resolved.json() as {roomId: string};
  return joinMatchToRoom(body, env, value.roomId,
    {fallbackCodeDigest: codeDigest});
}

async function matchControl(request: Request, env: Env, roomId: string,
                            action: "command" | "state"): Promise<Response> {
  const bearer = request.headers.get("authorization") || "";
  if (!/^Bearer [A-Za-z0-9_-]{43}$/.test(bearer) ||
      !(await validBoundCredential(env, "match-endpoint", roomId,
        bearer.slice(7)))) {
    return json({error: "unauthorized"}, 401);
  }
  /* Read and validate the body under the authenticated credential before the
   * reserve is charged; only then does this become billable work. */
  const body = await readJson<Record<string, unknown>>(request,
    MATCH_LIMITS.maxCommandBytes);
  if (action === "state" ? !exactKeys(body, []) : !validMatchCommandRequest(body)) {
    return json({error: action === "state" ? "invalid_request" : "invalid_command"},
      400);
  }
  const admission = await budget(env, "control", 2, "matchControl");
  if (!admission.ok) return admission;
  if (action === "state") {
    return matchStub(env, roomId).fetch("https://match/state", internalRequest({
      method: "POST", headers: {"content-type": "application/json",
        "x-match-credential-digest": await digest(
          env, "match-endpoint", bearer.slice(7))},
      body: new Uint8Array(),
    }));
  }
  return matchStub(env, roomId).fetch("https://match/command", internalRequest({
    method: "POST", headers: {"content-type": "application/json",
      "x-match-credential-digest": await digest(env, "match-endpoint", bearer.slice(7))},
    body: JSON.stringify(body),
  }));
}

async function rotateMatch(request: Request, env: Env,
                           roomId: string): Promise<Response> {
  const bearer = request.headers.get("authorization") || "";
  if (!/^Bearer [A-Za-z0-9_-]{43}$/.test(bearer) ||
      !(await validBoundCredential(env, "match-endpoint", roomId,
        bearer.slice(7)))) {
    return json({error: "unauthorized"}, 401);
  }
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeys(body, ["expectedInviteGeneration"])) {
    return json({error: "invalid_invite_generation"}, 400);
  }
  const expectedInviteGeneration = Number(body.expectedInviteGeneration);
  if (!Number.isInteger(expectedInviteGeneration) || expectedInviteGeneration < 1) {
    return json({error: "invalid_invite_generation"}, 400);
  }
  const roomBytes = fromBase64Url(roomId);
  if (!roomBytes || roomBytes.byteLength !== 16) return json({error: "not_found"}, 404);
  const admission = await budget(env, "control", 10, "matchRotate");
  if (!admission.ok) return admission;
  const capabilityBytes = new Uint8Array(32);
  capabilityBytes.set(roomBytes);
  capabilityBytes.set(crypto.getRandomValues(new Uint8Array(16)), 16);
  const capability = base64Url(capabilityBytes);
  for (let attempt = 0; attempt < 8; attempt++) {
    const code = fallbackCode();
    if (!(await registerCode(env, code, roomId, "match-v1", "match-code",
      MATCH_LIMITS.inviteTtlMs))) continue;
    const response = await matchStub(env, roomId).fetch("https://match/rotate",
      internalRequest({
      method: "POST", headers: {"content-type": "application/json",
        "x-match-credential-digest": await digest(env, "match-endpoint",
          bearer.slice(7))},
      body: JSON.stringify({inviteDigest: await digest(env, "match-invite", capability),
        fallbackCodeDigest: await digest(env, "match-code", code),
        expectedInviteGeneration, now: Date.now()}),
    }));
    if (!response.ok) return response;
    const state = await response.json() as Record<string, unknown>;
    const inviteExpiresInMs = inviteRemainingMs(state.inviteExpiresAt, Date.now());
    if (inviteExpiresInMs === null) {
      return json({error: "invite_expired"}, 409);
    }
    return json({...state, fallbackCode: code,
      inviteExpiresInMs,
      inviteUrl: `${env.PARTY_ORIGIN}/room/#match=${capability}`});
  }
  return json({error: "invite_rotation_failed"}, 503);
}

async function connectMatch(request: Request, env: Env, roomId: string): Promise<Response> {
  const credential = matchSocketCredential(request, "gb-match-v1");
  if (!credential ||
      !(await validBoundCredential(env, "match-endpoint", roomId, credential))) {
    return json({error: "unauthorized"}, 401);
  }
  const admission = await budget(env, "control", 4, "matchSocket");
  if (!admission.ok) return admission;
  const headers = new Headers(request.headers);
  headers.set("x-match-credential-digest", await digest(env, "match-endpoint", credential));
  headers.set("sec-websocket-protocol", "gb-match-v1");
  return matchStub(env, roomId).fetch("https://match/connect",
    internalRequest({headers}));
}

function matchSocketCredential(request: Request, version: string): string | null {
  const offered = (request.headers.get("sec-websocket-protocol") || "").split(",")
    .map(value => value.trim()).filter(Boolean);
  if (offered.length !== 2 || offered.filter(value => value === version).length !== 1) {
    return null;
  }
  const credential = offered.find(value => /^gb-match\.[A-Za-z0-9_-]{43}$/.test(value));
  return credential ? credential.slice("gb-match.".length) : null;
}

async function connectMatchSignal(request: Request, env: Env,
                                  roomId: string): Promise<Response> {
  const credential = matchSocketCredential(request, "gb-match-signal-v1");
  if (!credential ||
      !(await validBoundCredential(env, "match-endpoint", roomId, credential))) {
    return json({error: "unauthorized"}, 401);
  }
  // Budget + room upgrade + 13 request-equivalents for the hard lifetime cap
  // of 256 inbound signaling messages (billed in 20-message increments).
  const admission = await budget(env, "control", 15, "matchSignalSocket");
  if (!admission.ok) return admission;
  const headers = new Headers(request.headers);
  headers.set("x-match-credential-digest", await digest(env, "match-endpoint", credential));
  headers.set("sec-websocket-protocol", "gb-match-signal-v1");
  return matchStub(env, roomId).fetch("https://match/signal",
    internalRequest({headers}));
}

async function registerCode(env: Env, code: string, roomId: string,
                            directory = "v1", purpose = "party-code",
                            expiresInMs = LIMITS.inviteTtlMs): Promise<boolean> {
  const response = await codeStub(env, directory).fetch("https://code/register",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify({codeDigest: await digest(env, purpose, code), roomId,
      expiresAt: Date.now() + expiresInMs}),
  }));
  return response.ok;
}

async function createRoomForKey(hostPublicKey: string, env: Env,
                                pairingReserved = false,
                                sourceDigest = ""): Promise<Response> {
  /* Budget the worst collision path: budget DO + eight directory attempts +
   * one room initialize. Unused reservation is intentional $0 headroom. */
  if (!validPublicKey(hostPublicKey)) return json({error: "invalid_host_key"}, 400);
  if (!pairingReserved) {
    const admission = await budget(env, "pairing", 10, "partyCreate",
      sourceDigest);
    if (!admission.ok) return admission;
  }
  const roomBytes = crypto.getRandomValues(new Uint8Array(16));
  const secretBytes = crypto.getRandomValues(new Uint8Array(16));
  const capabilityBytes = new Uint8Array(32);
  capabilityBytes.set(roomBytes); capabilityBytes.set(secretBytes, 16);
  const capability = base64Url(capabilityBytes);
  const name = roomName(roomBytes);
  const hostCredential = await boundCredential(env, "host", name);
  let code = "";
  for (let attempt = 0; attempt < 8; attempt++) {
    const candidate = fallbackCode();
    if (await registerCode(env, candidate, name)) { code = candidate; break; }
  }
  if (!code) return json({error: "room_create_failed"}, 503);
  const inviteDigest = await digest(env, "invite", capability);
  const hostCredentialDigest = await digest(env, "host", hostCredential);
  const initialized = await roomStub(env, name).fetch("https://room/initialize",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify({roomId: name, inviteDigest, hostCredentialDigest,
      hostPublicKey,
      fallbackCodeDigest: await digest(env, "party-code", code), now: Date.now()}),
  }));
  if (!initialized.ok) return json({error: "room_create_failed"}, 503);
  const state = await initialized.json() as Record<string, unknown>;
  const inviteExpiresInMs = partyInviteRemainingMs(
    state.inviteExpiresAt, Date.now());
  if (inviteExpiresInMs === null) {
    return json({error: "room_create_failed"}, 503);
  }
  return json({roomId: name, hostCredential, fallbackCode: code,
    inviteGeneration: 1, inviteExpiresInMs,
    controllerUrl: `${env.PARTY_ORIGIN}/controller/#${capability}`}, 201);
}

async function createRoom(request: Request, env: Env): Promise<Response> {
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeys(body, ["hostPublicKey"]) || !validPublicKey(body.hostPublicKey)) {
    return json({error: "invalid_host_key"}, 400);
  }
  return createRoomForKey(body.hostPublicKey, env, false,
    await createSource(request, env));
}

async function redeemToRoom(body: Record<string, unknown>, env: Env, roomId: string,
                            proof: {inviteDigest?: string; fallbackCodeDigest?: string}): Promise<Response> {
  /* Both callers validate this before charging budget; kept as the last
   * defensive gate in front of the room object. */
  if (!validPublicKey(body.controllerPublicKey)) {
    return json({error: "invalid_controller_key"}, 400);
  }
  const controllerId = randomToken(16);
  const credential = await boundCredential(env, "controller", roomId);
  const input = {
    inviteDigest: proof.inviteDigest || "",
    fallbackCodeDigest: proof.fallbackCodeDigest || "",
    controllerId, credentialDigest: await digest(env, "controller", credential),
    name: normalizeName(body.name), controllerPublicKey: body.controllerPublicKey,
    protocol: Number(body.protocol), now: Date.now(),
  };
  const response = await roomStub(env, roomId).fetch("https://room/redeem",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify(input),
  }));
  if (!response.ok) return response;
  const result = await response.json() as Record<string, unknown>;
  return json({controllerId, credential, roomId, protocol: 2,
    hostPublicKey: result.hostPublicKey}, 201);
}

async function redeem(request: Request, env: Env): Promise<Response> {
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeysWithOptionalName(body,
      ["capability", "protocol", "controllerPublicKey"]) ||
      typeof body.capability !== "string") {
    return json({error: "invalid_invite"}, 400);
  }
  const decoded = fromBase64Url(body.capability);
  if (!decoded || decoded.byteLength !== 32) return json({error: "invalid_invite"}, 400);
  if (!validPublicKey(body.controllerPublicKey)) {
    return json({error: "invalid_controller_key"}, 400);
  }
  const admission = await budget(env, "pairing", 2, "partyLinkJoin");
  if (!admission.ok) return admission;
  return redeemToRoom(body, env, roomName(decoded),
    {inviteDigest: await digest(env, "invite", body.capability)});
}

async function redeemCode(request: Request, env: Env): Promise<Response> {
  const body = await readJson<Record<string, unknown>>(request);
  if (!exactKeysWithOptionalName(body,
      ["code", "protocol", "controllerPublicKey"])) {
    return json({error: "invalid_code"}, 400);
  }
  const code = typeof body.code === "string" ? body.code.replace(/\s/g, "") : "";
  if (!/^\d{6}$/.test(code)) return json({error: "invalid_code"}, 400);
  if (!validPublicKey(body.controllerPublicKey)) {
    return json({error: "invalid_controller_key"}, 400);
  }
  const admission = await budget(env, "pairing", 3, "partyCodeJoin");
  if (!admission.ok) return admission;
  const resolved = await codeStub(env).fetch("https://code/resolve",
    internalRequest({
    method: "POST", headers: {"content-type": "application/json"},
    body: JSON.stringify({codeDigest: await digest(env, "party-code", code),
      requesterDigest: await digest(env, "requester",
      request.headers.get("cf-connecting-ip") || "local-test")}),
  }));
  if (resolved.status === 429) return json({error: "rate_limited"}, 429);
  if (!resolved.ok) return json({error: "invalid_code"}, 404);
  const value = await resolved.json() as {roomId: string};
  return redeemToRoom(body, env, value.roomId,
    {fallbackCodeDigest: await digest(env, "party-code", code)});
}

async function rotateInvite(env: Env, roomId: string,
                            hostDigest: string,
                            expectedInviteGeneration: number): Promise<Response> {
  const roomBytes = fromBase64Url(roomId);
  if (!roomBytes || roomBytes.byteLength !== 16) return json({error: "not_found"}, 404);
  const capabilityBytes = new Uint8Array(32);
  capabilityBytes.set(roomBytes);
  capabilityBytes.set(crypto.getRandomValues(new Uint8Array(16)), 16);
  const capability = base64Url(capabilityBytes);
  for (let attempt = 0; attempt < 8; attempt++) {
    const code = fallbackCode();
    if (!(await registerCode(env, code, roomId))) continue;
    const response = await roomStub(env, roomId).fetch("https://room/rotate",
      internalRequest({
      method: "POST", headers: {"content-type": "application/json",
        "x-party-host-digest": hostDigest},
      body: JSON.stringify({inviteDigest: await digest(env, "invite", capability),
        fallbackCodeDigest: await digest(env, "party-code", code),
        expectedInviteGeneration}),
    }));
    if (!response.ok) return response;
    const state = await response.json() as Record<string, unknown>;
    const inviteExpiresInMs = partyInviteRemainingMs(
      state.inviteExpiresAt, Date.now());
    if (inviteExpiresInMs === null) {
      return json({error: "invite_expired"}, 409);
    }
    return json({fallbackCode: code,
      inviteGeneration: state.inviteGeneration, inviteExpiresInMs,
      controllerUrl: `${env.PARTY_ORIGIN}/controller/#${capability}`});
  }
  return json({error: "invite_rotation_failed"}, 503);
}

async function hostControl(request: Request, env: Env, roomId: string,
                           action: string): Promise<Response> {
  /* Rotation can probe the directory eight times, then mutates the room once.
   * Other controls fan out exactly once. */
  const bearer = request.headers.get("authorization") || "";
  if (!/^Bearer [A-Za-z0-9_-]{43}$/.test(bearer) ||
      !(await validBoundCredential(env, "host", roomId, bearer.slice(7)))) {
    return json({error: "unauthorized"}, 401);
  }
  const input = await readJson<Record<string, unknown>>(request);
  let expectedRotation = 0;
  if (action === "rotate") {
    expectedRotation = Number(input.expectedInviteGeneration);
    if (!exactKeys(input, ["expectedInviteGeneration"]) ||
        !Number.isInteger(expectedRotation) || expectedRotation < 1) {
      return json({error: "invalid_invite_generation"}, 400);
    }
  } else {
    const expectedKeys = action === "approve"
      ? ["controllerId", "seat"]
      : action === "reject" || action === "remove"
      ? ["controllerId"]
      : [];
    if (!exactKeys(input, expectedKeys)) {
      return json({error: "invalid_request"}, 400);
    }
  }
  /* Authenticated host and a well-formed control body: only now is this
   * billable work. */
  const admission = await budget(env, "control", action === "rotate" ? 10 : 2,
    action === "rotate" ? "partyRotate" : "partyControl");
  if (!admission.ok) return admission;
  const hostDigest = await digest(env, "host", bearer.slice(7));
  if (action === "rotate") {
    return rotateInvite(env, roomId, hostDigest, expectedRotation);
  }
  const body = JSON.stringify(input);
  return roomStub(env, roomId).fetch(`https://room/${action}`, internalRequest({
    method: "POST", headers: {"content-type": "application/json",
      "x-party-host-digest": hostDigest}, body,
  }));
}

async function connect(request: Request, env: Env, roomId: string,
                       nativeBootstrap = "", controlReserved = false): Promise<Response> {
  const offered = (request.headers.get("sec-websocket-protocol") || "").split(",")
    .map(value => value.trim());
  const credentialProtocol = offered.find(value =>
    value.startsWith("gb-host.") || value.startsWith("gb-controller."));
  if (!offered.includes("gb-control-v1") || !credentialProtocol) {
    return json({error: "unauthorized"}, 401);
  }
  const separator = credentialProtocol.indexOf(".");
  const role = credentialProtocol.slice(3, separator);
  const credential = credentialProtocol.slice(separator + 1);
  if (!/^[A-Za-z0-9_-]{43}$/.test(credential) ||
      !(await validBoundCredential(env, role, roomId, credential))) {
    return json({error: "unauthorized"}, 401);
  }
  if (!controlReserved) {
    const admission = await budget(env, "control", 28, "partySocket");
    if (!admission.ok) return admission;
  }
  const credentialDigest = await digest(env, role, credential);
  const headers = new Headers(request.headers);
  /* An external caller cannot smuggle a secret-bearing bootstrap message into
   * an ordinary authenticated room socket. Only nativeCreateSocket adds it. */
  headers.delete("x-mdkr-native-bootstrap");
  headers.set("sec-websocket-protocol", `gb-control-v1, gb-ticket.${credentialDigest}`);
  if (nativeBootstrap) headers.set("x-mdkr-native-bootstrap", nativeBootstrap);
  return roomStub(env, roomId).fetch("https://room/connect",
    internalRequest({headers}));
}

function nativeSocketProtocols(request: Request): {hostPublicKey: string} | null {
  if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") return null;
  const offered = (request.headers.get("sec-websocket-protocol") || "").split(",")
    .map(value => value.trim());
  if (offered.length !== 3 || !offered.includes("gb-native-host-v1") ||
      !offered.includes("gb-control-v1")) return null;
  const keys = offered.filter(value => value.startsWith("gb-key."));
  if (keys.length !== 1) return null;
  const hostPublicKey = keys[0]!.slice("gb-key.".length);
  return validPublicKey(hostPublicKey) ? {hostPublicKey} : null;
}

function nativeReconnectProtocols(request: Request): boolean {
  if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") return false;
  const offered = (request.headers.get("sec-websocket-protocol") || "").split(",")
    .map(value => value.trim());
  return offered.length === 3 && offered.includes("gb-native-host-v1") &&
    offered.includes("gb-control-v1") && offered.filter(value =>
      /^gb-host\.[A-Za-z0-9_-]{43}$/.test(value)).length === 1;
}

async function nativeCreateSocket(request: Request, env: Env,
                                  hostPublicKey: string): Promise<Response> {
  /* Reuse the public create/connect contracts exactly while giving native one
   * WSS dependency instead of introducing a second portable HTTP stack. */
  const pairing = await budget(env, "pairing", 10, "partyCreate",
    await createSource(request, env));
  if (!pairing.ok) return pairing;
  /* Reserve the full socket lifetime before creating durable room state. A
   * closed control reserve therefore cannot leave an unreachable orphan. */
  const socketControl = await budget(env, "control", 28, "partySocket");
  if (!socketControl.ok) return socketControl;
  const created = await createRoomForKey(hostPublicKey, env, true);
  if (!created.ok) return created;
  const value = await created.json() as Record<string, unknown>;
  const roomId = typeof value.roomId === "string" ? value.roomId : "";
  const credential = typeof value.hostCredential === "string"
    ? value.hostCredential : "";
  if (!/^[A-Za-z0-9_-]{22}$/.test(roomId) ||
      !/^[A-Za-z0-9_-]{43}$/.test(credential)) {
    return json({error: "room_create_failed"}, 503);
  }
  const bootstrap = base64Url(new TextEncoder().encode(JSON.stringify({
    type: "native_bootstrap", ...value,
  })));
  const headers = new Headers(request.headers);
  headers.set("sec-websocket-protocol",
    `gb-control-v1, gb-host.${credential}`);
  return connect(new Request(
    `https://party/api/party/${roomId}/connect`, {headers}), env, roomId,
    bootstrap, true);
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    /* This value is also interpolated into bearer-capability URLs. Refuse the
     * entire API before origin exceptions or Durable Object work if deployment
     * configuration is not one canonical HTTPS origin (or local loopback). */
    if (!validPartyOrigin(env.PARTY_ORIGIN)) {
      return json({error: "service_unavailable"}, 503);
    }
    const url = new URL(request.url);
    const partySocket = /^\/api\/party\/([A-Za-z0-9_-]{22})\/connect$/.exec(url.pathname);
    const matchSocket = /^\/api\/match\/([A-Za-z0-9_-]{22})\/connect$/.exec(url.pathname);
    const matchSignalSocket = /^\/api\/match\/([A-Za-z0-9_-]{22})\/signal$/.exec(
      url.pathname);
    const nativeSocket = url.pathname === "/api/party/native-create" &&
      request.method === "GET" && !request.headers.has("origin")
      ? nativeSocketProtocols(request) : null;
    const nativeReconnect = partySocket && request.method === "GET" &&
      !request.headers.has("origin") && nativeReconnectProtocols(request);
    const nativeMatchSocket = request.method === "GET" &&
      !request.headers.has("origin") && Boolean(
        (matchSocket && matchSocketCredential(request, "gb-match-v1")) ||
        (matchSignalSocket && matchSocketCredential(request, "gb-match-signal-v1")));
    /* Browsers always send Origin on WebSocket handshakes. Originless access is
     * reserved for the versioned native bootstrap shape; all ordinary HTTP and
     * sockets retain strict same-origin enforcement. */
    if (!allowedOrigin(request, env) && !nativeSocket && !nativeReconnect &&
        !nativeMatchSocket) {
      return json({error: "origin_forbidden"}, 403);
    }
    if (nativeSocket) {
      return nativeCreateSocket(request, env, nativeSocket.hostPublicKey);
    }
    if (url.pathname === "/api/ops/capacity" && request.method === "GET") {
      return operationsStatus(request, env, "status");
    }
    if (url.pathname === "/api/ops/health" && request.method === "GET") {
      return operationsStatus(request, env, "health");
    }
    if (partySocket && request.method === "GET") {
      // Includes budget lookup, room upgrade and the maximum 512 incoming
      // signaling messages (billed in 20-message increments). The conservative
      // reservation makes a slow valid-credential flood finite as well as
      // rate-limited.
      return connect(request, env, partySocket[1]!);
    }
    if (matchSocket && request.method === "GET") {
      // Match sockets are state subscriptions; their first client message is
      // rejected and closes the socket. Reserve budget + upgrade + message.
      return connectMatch(request, env, matchSocket[1]!);
    }
    if (matchSignalSocket && request.method === "GET") {
      return connectMatchSignal(request, env, matchSignalSocket[1]!);
    }
    if (request.method !== "POST") return json({error: "method_not_allowed"}, 405);
    try {
      if (url.pathname === "/api/party/create") return await createRoom(request, env);
      if (url.pathname === "/api/match/create") return await createMatch(request, env);
      if (url.pathname === "/api/match/join") return await joinMatch(request, env);
      if (url.pathname === "/api/match/code") return await joinMatchCode(request, env);
      if (url.pathname === "/api/controller/redeem") return await redeem(request, env);
      if (url.pathname === "/api/controller/code") return await redeemCode(request, env);
      const match = /^\/api\/party\/([A-Za-z0-9_-]{22})\/(approve|reject|remove|rotate|revoke|close)$/.exec(url.pathname);
      if (match) return await hostControl(request, env, match[1]!, match[2]!);
      const matchAction = /^\/api\/match\/([A-Za-z0-9_-]{22})\/(command|state)$/.exec(url.pathname);
      if (matchAction) return await matchControl(request, env, matchAction[1]!,
        matchAction[2] as "command" | "state");
      const matchRotate = /^\/api\/match\/([A-Za-z0-9_-]{22})\/rotate$/.exec(url.pathname);
      if (matchRotate) return await rotateMatch(request, env, matchRotate[1]!);
      return json({error: "not_found"}, 404);
    } catch (error) {
      if (error instanceof Response) {
        const id = await error.text().catch(() => "invalid_request");
        return json({error: id || "invalid_request"}, error.status);
      }
      return json({error: "service_unavailable"}, 503);
    }
  },
} satisfies ExportedHandler<Env>;
