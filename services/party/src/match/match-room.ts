import {DurableObject} from "cloudflare:workers";
import {constantTimeEqual, json, readJson, utf8Exceeds} from "../security";
import {rejectUnsupportedInternalApi} from "../internal-api";
import type {Env} from "../types";
import {blankCompatibility, MATCH_COMMAND_TYPES, MATCH_LIMITS,
  MATCH_PROTOCOL_VERSION, type MatchCommandV1, type MatchCompatibilityV1,
  type MatchCredential, type StoredMatchRoomV1, parseU64,
  validCompatibility, validMatchControlLog} from "./protocol";
import {createMatchLobby, dispatchMatchCommand, validMatchLobby} from "./reducer";

interface InitializeInput {
  roomNumericId: string;
  leaderEndpointId: string;
  leaderCredentialDigest: string;
  inviteDigest: string;
  fallbackCodeDigest: string;
  compatibility: MatchCompatibilityV1;
  leaderSeats: number;
  now: number;
}

interface JoinInput {
  inviteDigest: string;
  fallbackCodeDigest: string;
  endpointId: string;
  credentialDigest: string;
  compatibility: MatchCompatibilityV1;
  seatCount: number;
  commandId: string;
  now: number;
}

interface RotateInput {
  inviteDigest: string;
  fallbackCodeDigest: string;
  expectedInviteGeneration: number;
  now: number;
}

interface MatchSocketAttachment {
  endpointId: string;
}

function publicRoom(record: StoredMatchRoomV1): Record<string, unknown> {
  const {receipts: _receipts, nextReceipt: _nextReceipt, ...visibleLobby} =
    record.lobby;
  const members = record.lobby.members.map(member => {
    const {lastCommandId: _commandId,
      lastCommandFingerprint: _fingerprint, ...visible} = member;
    return visible;
  });
  return {type: "match_state", schemaVersion: record.schemaVersion,
    expiresAt: record.expiresAt, inviteExpiresAt: record.inviteExpiresAt,
    inviteGeneration: record.inviteGeneration,
    closedReason: record.closedReason, lobby: {...visibleLobby, members},
    controlTail: record.controlLog};
}

function appendControl(record: StoredMatchRoomV1,
                       value: StoredMatchRoomV1["controlLog"][number]): void {
  record.controlLog.push(value);
  if (record.controlLog.length > MATCH_LIMITS.controlLog) {
    record.controlLog.splice(0, record.controlLog.length - MATCH_LIMITS.controlLog);
  }
}

function validDigest(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{43}$/.test(value);
}

function validInitialize(value: InitializeInput): boolean {
  return parseU64(value.roomNumericId) !== null &&
    parseU64(value.leaderEndpointId) !== null && validDigest(value.inviteDigest) &&
    validDigest(value.fallbackCodeDigest) &&
    validDigest(value.leaderCredentialDigest) && validCompatibility(value.compatibility) &&
    Number.isInteger(value.leaderSeats) && value.leaderSeats >= 1 &&
    value.leaderSeats <= MATCH_LIMITS.maxSeatsPerEndpoint &&
    Number.isSafeInteger(value.now) && value.now > 0;
}

function commandFrom(value: Record<string, unknown>,
                     actorEndpointId: string): MatchCommandV1 | null {
  if (value.protocolVersion !== MATCH_PROTOCOL_VERSION ||
      !Number.isInteger(value.expectedRevision) || Number(value.expectedRevision) < 1 ||
      Number(value.expectedRevision) > 0xffff_ffff || parseU64(value.commandId) === null ||
      typeof value.type !== "string" ||
      !MATCH_COMMAND_TYPES.has(value.type as MatchCommandV1["type"]) ||
      !Number.isInteger(value.value) || Number(value.value) < 0 ||
      Number(value.value) > 0xffff_ffff ||
      parseU64(value.targetEndpointId, true) === null ||
      value.compatibility !== undefined) return null;
  return {protocolVersion: 1, expectedRevision: Number(value.expectedRevision),
    commandId: String(value.commandId), actorEndpointId,
    type: value.type as MatchCommandV1["type"], value: Number(value.value),
    targetEndpointId: String(value.targetEndpointId),
    compatibility: blankCompatibility()};
}

export class MatchRoom extends DurableObject<Env> {
  private async record(): Promise<StoredMatchRoomV1 | undefined> {
    const value = await this.ctx.storage.get<StoredMatchRoomV1>("match");
    if (!value) return undefined;
    const closed = value.lobby?.phase === "closed";
    if (value.schemaVersion !== 1 || !validMatchLobby(value.lobby) ||
        !Array.isArray(value.credentials) || !Array.isArray(value.controlLog) ||
        !Number.isSafeInteger(value.createdAt) || value.createdAt <= 0 ||
        !Number.isSafeInteger(value.expiresAt) || value.expiresAt <= value.createdAt ||
        !Number.isSafeInteger(value.inviteExpiresAt) ||
        value.inviteExpiresAt < value.createdAt ||
        value.inviteExpiresAt > value.expiresAt ||
        !Number.isInteger(value.inviteGeneration) || value.inviteGeneration < 1 ||
        value.inviteGeneration > 0xffff_ffff ||
        (closed ? (value.inviteDigest !== "" || value.fallbackCodeDigest !== "" ||
          !["host_closed", "room_expired"].includes(value.closedReason || "")) :
          (!validDigest(value.inviteDigest) || !validDigest(value.fallbackCodeDigest) ||
           value.closedReason !== null)) ||
        value.credentials.length !== value.lobby.members.length ||
        !validMatchControlLog(value.controlLog, value.lobby.revision,
          value.lobby.matchEpoch) ||
        value.credentials.some(item => !parseU64(item.endpointId) ||
          !validDigest(item.digest) || !Number.isSafeInteger(item.issuedAt) ||
          item.issuedAt < value.createdAt || item.issuedAt > value.expiresAt ||
          !value.lobby.members.some(member => member.endpointId === item.endpointId)) ||
        new Set(value.credentials.map(item => item.endpointId)).size !==
          value.credentials.length ||
        new Set(value.credentials.map(item => item.digest)).size !==
          value.credentials.length) {
      throw new Error("unsupported match room schema");
    }
    return value;
  }

  private credential(request: Request, record: StoredMatchRoomV1): MatchCredential | undefined {
    const supplied = request.headers.get("x-match-credential-digest") || "";
    return record.credentials.find(item => constantTimeEqual(supplied, item.digest));
  }

  private async save(record: StoredMatchRoomV1): Promise<void> {
    await this.ctx.storage.put("match", record);
  }

  override async fetch(request: Request): Promise<Response> {
    const rejected = rejectUnsupportedInternalApi(request);
    if (rejected) return rejected;
    const url = new URL(request.url);
    if (request.method !== "POST" && url.pathname !== "/connect") {
      return json({error: "method_not_allowed"}, 405);
    }
    if (url.pathname === "/initialize") {
      if (await this.record()) return json({error: "already_initialized"}, 409);
      const input = await readJson<InitializeInput>(request);
      if (!validInitialize(input)) return json({error: "invalid_match"}, 400);
      const lobby = createMatchLobby(input.roomNumericId, input.leaderEndpointId,
        input.compatibility, input.leaderSeats);
      if (!lobby) return json({error: "invalid_match"}, 400);
      const record: StoredMatchRoomV1 = {schemaVersion: 1, createdAt: input.now,
        expiresAt: input.now + MATCH_LIMITS.roomTtlMs,
        inviteExpiresAt: input.now + MATCH_LIMITS.inviteTtlMs,
        inviteGeneration: 1,
        inviteDigest: input.inviteDigest,
        fallbackCodeDigest: input.fallbackCodeDigest, lobby,
        credentials: [{endpointId: input.leaderEndpointId,
          digest: input.leaderCredentialDigest, issuedAt: input.now}],
        controlLog: [], closedReason: null};
      await this.save(record);
      await this.ctx.storage.setAlarm(record.expiresAt);
      return json(publicRoom(record), 201);
    }
    const record = await this.record();
    if (!record) return json({error: "not_found"}, 404);
    if (Date.now() >= record.expiresAt) {
      for (const socket of this.ctx.getWebSockets()) socket.close(4000, "room_expired");
      await this.ctx.storage.deleteAll();
      return json({error: "not_found"}, 404);
    }
    if (url.pathname === "/join") {
      const input = await readJson<JoinInput>(request);
      if ((!validDigest(input.inviteDigest) && !validDigest(input.fallbackCodeDigest)) ||
          !validDigest(input.credentialDigest) ||
          !validCompatibility(input.compatibility) || !parseU64(input.endpointId) ||
          !parseU64(input.commandId) || !Number.isSafeInteger(input.now) ||
          !Number.isInteger(input.seatCount)) return json({error: "invalid_join"}, 400);
      const inviteMatches = validDigest(input.inviteDigest) &&
        constantTimeEqual(input.inviteDigest, record.inviteDigest);
      const codeMatches = validDigest(input.fallbackCodeDigest) &&
        constantTimeEqual(input.fallbackCodeDigest, record.fallbackCodeDigest);
      if (record.lobby.phase !== "lobby" || input.now >= record.inviteExpiresAt ||
          (!inviteMatches && !codeMatches)) {
        return json({error: input.now >= record.inviteExpiresAt
          ? "invite_expired" : "invalid_invite"}, 409);
      }
      if (record.lobby.revision >= MATCH_LIMITS.maxTransitions) {
        return json({error: "service_budget_safe"}, 503);
      }
      if (record.credentials.some(item => item.endpointId === input.endpointId ||
          constantTimeEqual(item.digest, input.credentialDigest)) ||
          record.lobby.receipts.some(item => item.actorEndpointId === input.endpointId)) {
        return json({error: "credential_conflict"}, 409);
      }
      const command: MatchCommandV1 = {protocolVersion: 1,
        expectedRevision: record.lobby.revision, commandId: input.commandId,
        actorEndpointId: input.endpointId, type: "join", value: input.seatCount,
        targetEndpointId: "0", compatibility: input.compatibility};
      const result = dispatchMatchCommand(record.lobby, command);
      if (!result.accepted) return json({error: result.error}, 409);
      record.credentials.push({endpointId: input.endpointId,
        digest: input.credentialDigest, issuedAt: input.now});
      appendControl(record, result);
      await this.save(record);
      this.broadcast(record);
      return json({endpointId: input.endpointId, ...publicRoom(record)}, 201);
    }
    if (url.pathname === "/connect") return this.upgrade(request, record);
    const credential = this.credential(request, record);
    if (!credential) return json({error: "unauthorized"}, 401);
    if (url.pathname === "/state") return json(publicRoom(record));
    if (url.pathname === "/rotate") {
      const input = await readJson<RotateInput>(request);
      if (!validDigest(input.inviteDigest) || !validDigest(input.fallbackCodeDigest) ||
          !Number.isInteger(input.expectedInviteGeneration) ||
          !Number.isSafeInteger(input.now) || input.now < record.createdAt) {
        return json({error: "invalid_invite"}, 400);
      }
      if (credential.endpointId !== record.lobby.leaderEndpointId) {
        return json({error: "unauthorized"}, 403);
      }
      if (record.lobby.phase !== "lobby" ||
          input.expectedInviteGeneration !== record.inviteGeneration ||
          record.inviteGeneration === 0xffff_ffff) {
        return json({error: "invalid_state"}, 409);
      }
      record.inviteDigest = input.inviteDigest;
      record.fallbackCodeDigest = input.fallbackCodeDigest;
      record.inviteGeneration++;
      record.inviteExpiresAt = Math.min(input.now + MATCH_LIMITS.inviteTtlMs,
        record.expiresAt);
      await this.save(record);
      this.broadcast(record);
      return json(publicRoom(record));
    }
    if (url.pathname !== "/command") return json({error: "not_found"}, 404);
    if (record.lobby.revision >= MATCH_LIMITS.maxTransitions) {
      return json({error: "service_budget_safe"}, 503);
    }
    const body = await readJson<Record<string, unknown>>(request,
      MATCH_LIMITS.maxCommandBytes);
    const command = commandFrom(body, credential.endpointId);
    if (!command) return json({error: "invalid_command"}, 400);
    const before = record.lobby.revision;
    const result = dispatchMatchCommand(record.lobby, command);
    if (!result.accepted) return json({error: result.error,
      revision: result.revision}, result.error === "stale_revision" ? 412 : 409);
    if (!result.duplicate) {
      appendControl(record, result);
      if (command.type === "leave") {
        record.credentials = record.credentials.filter(item =>
          item.endpointId !== credential.endpointId);
      } else if (command.type === "close") {
        record.closedReason = "host_closed";
        record.inviteDigest = "";
        record.fallbackCodeDigest = "";
        record.inviteExpiresAt = Date.now();
      }
      await this.save(record);
      this.broadcast(record);
      if (command.type === "close") {
        for (const socket of this.ctx.getWebSockets()) socket.close(4000, "host_closed");
      }
    }
    return json({...result, previousRevision: before});
  }

  private async upgrade(request: Request, record: StoredMatchRoomV1): Promise<Response> {
    if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return json({error: "upgrade_required"}, 426);
    }
    const credential = this.credential(request, record);
    if (!credential) return json({error: "unauthorized"}, 401);
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    const attachment: MatchSocketAttachment = {endpointId: credential.endpointId};
    server.serializeAttachment(attachment);
    this.ctx.acceptWebSocket(server, [credential.endpointId]);
    server.send(JSON.stringify(publicRoom(record)));
    return new Response(null, {status: 101, webSocket: client,
      headers: {"sec-websocket-protocol": "gb-match-v1"}});
  }

  override webSocketMessage(socket: WebSocket,
                            message: string | ArrayBuffer): void {
    if ((typeof message === "string" &&
         utf8Exceeds(message, MATCH_LIMITS.maxSocketMessageBytes)) ||
        (typeof message !== "string" &&
         message.byteLength > MATCH_LIMITS.maxSocketMessageBytes)) {
      socket.close(4009, "message_too_large");
      return;
    }
    socket.close(4003, "commands_use_authenticated_http");
  }
  override webSocketClose(): void {}
  override webSocketError(): void {}

  private broadcast(record: StoredMatchRoomV1): void {
    const message = JSON.stringify(publicRoom(record));
    for (const socket of this.ctx.getWebSockets()) {
      if (socket.readyState === WebSocket.OPEN) socket.send(message);
    }
  }

  override async alarm(): Promise<void> {
    const record = await this.record();
    if (record) {
      record.lobby.phase = "closed";
      record.closedReason = "room_expired";
      for (const socket of this.ctx.getWebSockets()) socket.close(4000, "room_expired");
    }
    await this.ctx.storage.deleteAll();
  }
}
