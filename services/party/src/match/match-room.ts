import {DurableObject} from "cloudflare:workers";
import {constantTimeEqual, json, readJson, utf8Exceeds} from "../security";
import {rejectUnsupportedInternalApi} from "../internal-api";
import type {Env} from "../types";
import {blankCompatibility, MATCH_LIMITS,
  MATCH_PROTOCOL_VERSION, type MatchCommandV1, type MatchCompatibilityV1,
  type MatchCredential, type StoredMatchRoomV1, parseU64,
  validCompatibility, validMatchCommandRequest,
  validMatchControlLog} from "./protocol";
import {createMatchLobby, dispatchMatchCommand, validMatchLobby} from "./reducer";
import {admitMatchSignalMessage, parseMatchSignalMessage,
  type MatchSignalRateState} from "./signaling";

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

interface MatchStateSocketAttachment {
  kind?: "state";
  endpointId: string;
}

interface MatchSignalSocketAttachment extends MatchSignalRateState {
  kind: "signal";
  endpointId: string;
  connectionGeneration: number;
  lastSequence: number;
}

type MatchSocketAttachment = MatchStateSocketAttachment | MatchSignalSocketAttachment;

function closeSocket(socket: WebSocket, code: number, reason: string): void {
  try { socket.close(code, reason); }
  catch { /* A broken peer must not abort storage cleanup or another delivery. */ }
}

/* WebSocket delivery is best effort after authoritative state has committed.
 * A single broken hibernated socket must never turn a successful command into
 * a 503, prevent later peers receiving the state, or abort room cleanup. */
export function deliverMatchSocketText(socket: WebSocket, message: string,
                                       failureReason: string): boolean {
  if (socket.readyState !== WebSocket.OPEN) return false;
  try {
    socket.send(message);
    return true;
  } catch {
    closeSocket(socket, 1011, failureReason);
    return false;
  }
}

function socketAttachment(socket: WebSocket): MatchSocketAttachment | null {
  let value: Partial<MatchSocketAttachment> | null;
  try {
    value = socket.deserializeAttachment() as Partial<MatchSocketAttachment> | null;
  } catch {
    return null;
  }
  if (!value || parseU64(value.endpointId) === null) return null;
  if (value.kind === undefined || value.kind === "state") {
    return {kind: "state", endpointId: value.endpointId!};
  }
  const signal = value as Partial<MatchSignalSocketAttachment>;
  if (signal.kind !== "signal" || !Number.isInteger(signal.connectionGeneration) ||
      signal.connectionGeneration! < 1 || signal.connectionGeneration! > 0xffff_ffff ||
      !Number.isInteger(signal.lastSequence) || signal.lastSequence! < 0 ||
      signal.lastSequence! > 0xffff_ffff || !Number.isSafeInteger(signal.messages) ||
      signal.messages! < 0 || !Number.isSafeInteger(signal.windowStartedAt) ||
      signal.windowStartedAt! < 0 || !Number.isSafeInteger(signal.lifetimeMessages) ||
      signal.lifetimeMessages! < 0) return null;
  return signal as MatchSignalSocketAttachment;
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
  if (!validMatchCommandRequest(value)) return null;
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

  override fetch(request: Request): Promise<Response> {
    /* Durable Object events may interleave at any await. Keep the complete
     * read/authorize/reduce/persist/broadcast transition inside one input
     * gate so two callers cannot both commit from the same revision. */
    return this.ctx.blockConcurrencyWhile(() => this.fetchSerialized(request));
  }

  private async fetchSerialized(request: Request): Promise<Response> {
    const rejected = rejectUnsupportedInternalApi(request);
    if (rejected) return rejected;
    const url = new URL(request.url);
    if (request.method !== "POST" && url.pathname !== "/connect" &&
        url.pathname !== "/signal") {
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
      for (const socket of this.ctx.getWebSockets())
        closeSocket(socket, 4000, "room_expired");
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
    if (url.pathname === "/connect") return this.upgradeState(request, record);
    if (url.pathname === "/signal") return this.upgradeSignal(request, record);
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
      if (command.type === "leave") {
        this.closeEndpointSockets(credential.endpointId, false, 4001, "membership_ended");
      } else if (command.type === "disconnect") {
        this.closeEndpointSockets(credential.endpointId, true, 4001, "peer_disconnected");
      }
      if (command.type === "close") {
        for (const socket of this.ctx.getWebSockets())
          closeSocket(socket, 4000, "host_closed");
      }
    }
    return json({...result, previousRevision: before});
  }

  private async upgradeState(request: Request, record: StoredMatchRoomV1): Promise<Response> {
    if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return json({error: "upgrade_required"}, 426);
    }
    const credential = this.credential(request, record);
    if (!credential) return json({error: "unauthorized"}, 401);
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    const attachment: MatchStateSocketAttachment = {kind: "state",
      endpointId: credential.endpointId};
    try {
      server.serializeAttachment(attachment);
      this.ctx.acceptWebSocket(server,
        ["match-state", `endpoint:${credential.endpointId}`]);
    } catch {
      closeSocket(server, 1011, "state_socket_setup_failed");
      return json({error: "service_unavailable"}, 503);
    }
    if (!deliverMatchSocketText(server, JSON.stringify(publicRoom(record)),
      "state_delivery_failed")) {
      /* The server endpoint is already accepted; answer the negotiated upgrade
       * and let the client observe the typed close/reconnect instead of trying
       * to return ordinary HTTP after the object owns the socket. */
      return new Response(null, {status: 101, webSocket: client,
        headers: {"sec-websocket-protocol": "gb-match-v1"}});
    }
    return new Response(null, {status: 101, webSocket: client,
      headers: {"sec-websocket-protocol": "gb-match-v1"}});
  }

  private signalSockets(endpointId?: string): Array<{
      socket: WebSocket; attachment: MatchSignalSocketAttachment}> {
    const found: Array<{socket: WebSocket; attachment: MatchSignalSocketAttachment}> = [];
    for (const socket of this.ctx.getWebSockets()) {
      const attachment = socketAttachment(socket);
      if (attachment?.kind === "signal" &&
          (endpointId === undefined || attachment.endpointId === endpointId)) {
        found.push({socket, attachment});
      }
    }
    return found;
  }

  private sendSignalPresence(endpointId: string, connectionGeneration: number,
                             present: boolean, except?: WebSocket): void {
    const message = JSON.stringify({protocolVersion: 1, type: "peer_presence",
      endpointId, connectionGeneration, present});
    for (const target of this.signalSockets()) {
      if (target.attachment.endpointId !== endpointId &&
          target.socket !== except && target.socket.readyState === WebSocket.OPEN) {
        deliverMatchSocketText(target.socket, message, "signal_delivery_failed");
      }
    }
  }

  private async upgradeSignal(request: Request,
                              record: StoredMatchRoomV1): Promise<Response> {
    if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return json({error: "upgrade_required"}, 426);
    }
    const credential = this.credential(request, record);
    if (!credential) return json({error: "unauthorized"}, 401);
    const member = record.lobby.members.find(item =>
      item.endpointId === credential.endpointId);
    if (record.lobby.phase === "closed" || !member?.connected) {
      return json({error: "invalid_state"}, 409);
    }
    const key = `signal-sequence:${credential.endpointId}`;
    const previous = await this.ctx.storage.get<number>(key) || 0;
    if (!Number.isInteger(previous) || previous < 0 || previous >= 0xffff_ffff) {
      return json({error: "service_budget_safe"}, 503);
    }
    const connectionGeneration = previous + 1;
    const replaced = this.signalSockets(credential.endpointId);
    /* A replacement close is asynchronous. Canonicalize transiently
     * overlapping sockets to one highest generation per peer so a concurrent
     * welcome can never expose duplicate endpoint ids and make an honest
     * launcher fail its exact-schema check. */
    const peerGenerations = new Map<string, number>();
    for (const item of this.signalSockets()) {
      if (item.attachment.endpointId === credential.endpointId ||
          item.socket.readyState !== WebSocket.OPEN) continue;
      const prior = peerGenerations.get(item.attachment.endpointId) || 0;
      if (item.attachment.connectionGeneration > prior) {
        peerGenerations.set(item.attachment.endpointId,
          item.attachment.connectionGeneration);
      }
    }
    const peers = [...peerGenerations]
      .map(([endpointId, peerGeneration]) => ({endpointId,
        connectionGeneration: peerGeneration}))
      .sort((left, right) => BigInt(left.endpointId) < BigInt(right.endpointId) ? -1 :
        BigInt(left.endpointId) > BigInt(right.endpointId) ? 1 : 0);
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    const attachment: MatchSignalSocketAttachment = {kind: "signal",
      endpointId: credential.endpointId, connectionGeneration,
      messages: 0, windowStartedAt: Date.now(), lifetimeMessages: 0,
      lastSequence: 0};
    try {
      server.serializeAttachment(attachment);
      this.ctx.acceptWebSocket(server,
        ["match-signal", `signal:${credential.endpointId}`]);
    } catch {
      closeSocket(server, 1011, "signal_socket_setup_failed");
      return json({error: "service_unavailable"}, 503);
    }
    if (!deliverMatchSocketText(server,
      JSON.stringify({protocolVersion: 1, type: "signal_welcome",
        endpointId: credential.endpointId, connectionGeneration, peers}),
      "signal_delivery_failed")) {
      return new Response(null, {status: 101, webSocket: client,
        headers: {"sec-websocket-protocol": "gb-match-signal-v1"}});
    }
    try {
      await this.ctx.storage.put(key, connectionGeneration);
    } catch {
      closeSocket(server, 1011, "signal_generation_failed");
      return json({error: "service_unavailable"}, 503);
    }
    for (const existing of replaced)
      closeSocket(existing.socket, 4001, "connection_replaced");
    this.sendSignalPresence(credential.endpointId, connectionGeneration, true, server);
    return new Response(null, {status: 101, webSocket: client,
      headers: {"sec-websocket-protocol": "gb-match-signal-v1"}});
  }

  override async webSocketMessage(socket: WebSocket,
                                  message: string | ArrayBuffer): Promise<void> {
    const attachment = socketAttachment(socket);
    if (!attachment) { closeSocket(socket, 4001, "unauthorized"); return; }
    const maximum = attachment.kind === "signal" ? MATCH_LIMITS.maxSignalMessageBytes :
      MATCH_LIMITS.maxSocketMessageBytes;
    if ((typeof message === "string" && utf8Exceeds(message, maximum)) ||
        (typeof message !== "string" && message.byteLength > maximum)) {
      closeSocket(socket, 4009, "message_too_large");
      return;
    }
    if (attachment.kind !== "signal") {
      closeSocket(socket, 4003, "commands_use_authenticated_http");
      return;
    }
    if (typeof message !== "string") {
      closeSocket(socket, 4003, "signaling_text_only");
      return;
    }
    const now = Date.now();
    if (!admitMatchSignalMessage(attachment, now)) {
      closeSocket(socket, 4008, "rate_limited");
      return;
    }
    let raw: unknown;
    try { raw = JSON.parse(message); }
    catch { closeSocket(socket, 4003, "invalid_signaling"); return; }
    const value = parseMatchSignalMessage(raw, attachment.endpointId);
    if (!value) { closeSocket(socket, 4003, "invalid_signaling"); return; }
    if (value.sequence <= attachment.lastSequence) {
      closeSocket(socket, 4003, "invalid_sequence");
      return;
    }
    attachment.lastSequence = value.sequence;
    try { socket.serializeAttachment(attachment); }
    catch { closeSocket(socket, 1011, "attachment_update_failed"); return; }
    await this.ctx.blockConcurrencyWhile(async () => {
      const currentSourceGeneration = await this.ctx.storage.get<number>(
        `signal-sequence:${attachment.endpointId}`);
      /* A message event can already be queued when a replacement upgrade closes
       * the old socket. Re-authorize the generation inside the same gate as
       * membership and target routing so that queued stale work cannot reach a
       * peer after the replacement presence announcement. */
      if (currentSourceGeneration !== attachment.connectionGeneration) {
        closeSocket(socket, 4001, "connection_replaced");
        return;
      }
      const record = await this.record();
      const source = record?.lobby.members.find(item =>
        item.endpointId === attachment.endpointId);
      const destination = record?.lobby.members.find(item =>
        item.endpointId === value.toEndpointId);
      const target = this.signalSockets(value.toEndpointId).find(item =>
        item.attachment.connectionGeneration === value.toConnectionGeneration &&
        item.socket.readyState === WebSocket.OPEN);
      if (!record || record.lobby.phase === "closed" || !source?.connected) {
        closeSocket(socket, 4001, "membership_ended");
        return;
      }
      const unavailable = () => {
        if (socket.readyState === WebSocket.OPEN) {
          deliverMatchSocketText(socket,
            JSON.stringify({protocolVersion: 1, type: "signal_error",
              sequence: value.sequence, toEndpointId: value.toEndpointId,
              toConnectionGeneration: value.toConnectionGeneration,
              error: "peer_unavailable"}), "signal_delivery_failed");
        }
      };
      if (!destination?.connected || !target) {
        unavailable();
        return;
      }
      const {toEndpointId, toConnectionGeneration, ...payload} = value;
      if (!deliverMatchSocketText(target.socket,
        JSON.stringify({...payload, toEndpointId,
          toConnectionGeneration, fromEndpointId: attachment.endpointId,
          fromConnectionGeneration: attachment.connectionGeneration}),
        "signal_delivery_failed")) {
        unavailable();
      }
    });
  }
  override async webSocketClose(socket: WebSocket): Promise<void> {
    const attachment = socketAttachment(socket);
    if (attachment?.kind !== "signal") return;
    await this.ctx.blockConcurrencyWhile(async () => {
      const replacement = this.signalSockets(attachment.endpointId).some(item =>
        item.socket !== socket && item.socket.readyState === WebSocket.OPEN);
      if (replacement) return;
      const current = await this.ctx.storage.get<number>(
        `signal-sequence:${attachment.endpointId}`);
      /* Keep the current-generation read and absence broadcast in the same
       * input gate as replacement upgrades. Otherwise this callback can read
       * its old generation, yield, then publish stale absence after a successor
       * has already published presence. */
      if (current === attachment.connectionGeneration) {
        this.sendSignalPresence(attachment.endpointId,
          attachment.connectionGeneration, false, socket);
      }
    });
  }
  override webSocketError(socket: WebSocket): Promise<void> {
    return this.webSocketClose(socket);
  }

  private closeEndpointSockets(endpointId: string, signalOnly: boolean,
                               code: number, reason: string): void {
    for (const socket of this.ctx.getWebSockets()) {
      const attachment = socketAttachment(socket);
      if (attachment?.endpointId === endpointId &&
          (!signalOnly || attachment.kind === "signal")) closeSocket(socket, code, reason);
    }
  }

  private broadcast(record: StoredMatchRoomV1): void {
    const message = JSON.stringify(publicRoom(record));
    for (const socket of this.ctx.getWebSockets()) {
      const attachment = socketAttachment(socket);
      if (attachment?.kind === "state" && socket.readyState === WebSocket.OPEN) {
        deliverMatchSocketText(socket, message, "state_delivery_failed");
      }
    }
  }

  override async alarm(): Promise<void> {
    const record = await this.record();
    if (record) {
      record.lobby.phase = "closed";
      record.closedReason = "room_expired";
      for (const socket of this.ctx.getWebSockets())
        closeSocket(socket, 4000, "room_expired");
    }
    await this.ctx.storage.deleteAll();
  }
}
