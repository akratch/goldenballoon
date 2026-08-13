import {DurableObject} from "cloudflare:workers";
import {applyRoomCommand} from "./room-model";
import {base64Url, constantTimeEqual, digest, fromBase64Url, json,
  readJson, utf8Exceeds, validPartyOrigin} from "./security";
import {internalRequest, rejectUnsupportedInternalApi} from "./internal-api";
import {LIMITS, type CreateRoomInput, type Env, type RedeemInput,
  type StoredRoom} from "./types";

interface SocketAttachment {
  role: "host" | "controller";
  controllerId?: string;
  messages: number;
  windowStartedAt: number;
  lifetimeMessages?: number;
}

function objectRecord(value: unknown): value is Record<string, unknown> {
  return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}

function exactKeys(value: Record<string, unknown>, expected: readonly string[]): boolean {
  const keys = Object.keys(value).sort();
  const wanted = [...expected].sort();
  return keys.length === wanted.length &&
    keys.every((key, index) => key === wanted[index]);
}

function positiveU32(value: unknown): value is number {
  return Number.isInteger(value) && Number(value) >= 1 && Number(value) <= 0xffff_ffff;
}

function normalizedDescription(value: unknown,
                               expectedType: "offer" | "answer"):
    {type: "offer" | "answer"; sdp: string} | null {
  if (!objectRecord(value) || !exactKeys(value, ["sdp", "type"]) ||
      value.type !== expectedType || typeof value.sdp !== "string" ||
      value.sdp.length === 0 || utf8Exceeds(value.sdp, 60 * 1024)) return null;
  return {type: expectedType, sdp: value.sdp};
}

function normalizedCandidate(value: unknown): Record<string, unknown> | null {
  if (!objectRecord(value)) return null;
  const allowed = new Set(["candidate", "sdpMid", "sdpMLineIndex",
    "usernameFragment"]);
  const keys = Object.keys(value);
  if (!keys.includes("candidate") || keys.some(key => !allowed.has(key)) ||
      typeof value.candidate !== "string" || value.candidate.length === 0 ||
      utf8Exceeds(value.candidate, 4 * 1024)) return null;
  if (value.sdpMid !== undefined && value.sdpMid !== null &&
      (typeof value.sdpMid !== "string" || utf8Exceeds(value.sdpMid, 256))) return null;
  if (value.sdpMLineIndex !== undefined && value.sdpMLineIndex !== null &&
      (!Number.isInteger(value.sdpMLineIndex) || Number(value.sdpMLineIndex) < 0 ||
       Number(value.sdpMLineIndex) > 0xffff)) return null;
  if (value.usernameFragment !== undefined && value.usernameFragment !== null &&
      (typeof value.usernameFragment !== "string" ||
       utf8Exceeds(value.usernameFragment, 256))) return null;
  return {
    candidate: value.candidate,
    ...(value.sdpMid === undefined ? {} : {sdpMid: value.sdpMid}),
    ...(value.sdpMLineIndex === undefined
      ? {} : {sdpMLineIndex: value.sdpMLineIndex}),
    ...(value.usernameFragment === undefined
      ? {} : {usernameFragment: value.usernameFragment}),
  };
}

/** Validate and rebuild transient signaling so an authenticated peer cannot
 * smuggle unknown fields to the other role. Controller identity always comes
 * from its socket attachment; the supplied field is syntax only. */
function normalizedPartySignal(value: unknown,
                               attachment: SocketAttachment):
    Record<string, unknown> | null {
  if (!objectRecord(value) || typeof value.type !== "string") return null;
  if (attachment.role === "host") {
    if (value.type === "webrtc_offer") {
      if (!exactKeys(value, ["peerGeneration", "sdp", "to", "type"]) ||
          typeof value.to !== "string" ||
          !/^[A-Za-z0-9_-]{22}$/.test(value.to) ||
          !positiveU32(value.peerGeneration)) return null;
      const sdp = normalizedDescription(value.sdp, "offer");
      return sdp ? {type: value.type, to: value.to,
        peerGeneration: value.peerGeneration, sdp} : null;
    }
    if (value.type === "webrtc_ice") {
      if (!exactKeys(value, ["candidate", "peerGeneration", "to", "type"]) ||
          typeof value.to !== "string" ||
          !/^[A-Za-z0-9_-]{22}$/.test(value.to) ||
          !positiveU32(value.peerGeneration)) return null;
      const candidate = normalizedCandidate(value.candidate);
      return candidate ? {type: value.type, to: value.to,
        peerGeneration: value.peerGeneration, candidate} : null;
    }
    return null;
  }
  if (typeof value.controllerId !== "string" ||
      !/^[A-Za-z0-9_-]{22}$/.test(value.controllerId)) return null;
  const controllerId = attachment.controllerId!;
  if (value.type === "controller_hello") {
    return exactKeys(value, ["controllerId", "type"])
      ? {type: value.type, controllerId} : null;
  }
  if (value.type === "webrtc_answer") {
    if (!exactKeys(value, ["controllerId", "peerGeneration", "sdp", "type"]) ||
        !positiveU32(value.peerGeneration)) return null;
    const sdp = normalizedDescription(value.sdp, "answer");
    return sdp ? {type: value.type, controllerId,
      peerGeneration: value.peerGeneration, sdp} : null;
  }
  if (value.type === "webrtc_ice") {
    if (!exactKeys(value,
        ["candidate", "controllerId", "peerGeneration", "type"]) ||
        !positiveU32(value.peerGeneration)) return null;
    const candidate = normalizedCandidate(value.candidate);
    return candidate ? {type: value.type, controllerId,
      peerGeneration: value.peerGeneration, candidate} : null;
  }
  return null;
}

function validNativeHostCommandShape(value: Record<string, unknown>): boolean {
  if (value.type !== "host_command" || typeof value.action !== "string") return false;
  if (value.action === "approve") {
    return exactKeys(value, ["action", "controllerId", "seat", "type"]) &&
      typeof value.controllerId === "string" &&
      /^[A-Za-z0-9_-]{22}$/.test(value.controllerId) &&
      Number.isInteger(value.seat) && Number(value.seat) >= 1 &&
      Number(value.seat) <= LIMITS.maxSeats;
  }
  if (value.action === "reject" || value.action === "remove") {
    return exactKeys(value, ["action", "controllerId", "type"]) &&
      typeof value.controllerId === "string" &&
      /^[A-Za-z0-9_-]{22}$/.test(value.controllerId);
  }
  if (value.action === "rotate") {
    return exactKeys(value, ["action", "expectedInviteGeneration", "type"]) &&
      positiveU32(value.expectedInviteGeneration);
  }
  return (value.action === "revoke" || value.action === "close") &&
    exactKeys(value, ["action", "type"]);
}

function closePartySocket(socket: WebSocket, code: number, reason: string): void {
  try { socket.close(code, reason); }
  catch { /* A broken peer cannot abort state cleanup or another delivery. */ }
}

/* Room state is authoritative once storage commits. Socket delivery afterward
 * is best effort: a stale hibernated peer must not turn a successful command
 * into an apparent HTTP failure or prevent a healthy peer receiving it. */
export function deliverPartySocketText(socket: WebSocket, message: string,
                                       failureReason: string): boolean {
  if (socket.readyState !== WebSocket.OPEN) return false;
  try {
    socket.send(message);
    return true;
  } catch {
    closePartySocket(socket, 1011, failureReason);
    return false;
  }
}

function socketAttachment(socket: WebSocket): SocketAttachment | null {
  let value: Partial<SocketAttachment> | null;
  try {
    value = socket.deserializeAttachment() as Partial<SocketAttachment> | null;
  } catch {
    return null;
  }
  if (!value || (value.role !== "host" && value.role !== "controller") ||
      !Number.isSafeInteger(value.messages) || value.messages! < 0 ||
      !Number.isSafeInteger(value.windowStartedAt) || value.windowStartedAt! < 0 ||
      (value.lifetimeMessages !== undefined &&
       (!Number.isSafeInteger(value.lifetimeMessages) || value.lifetimeMessages < 0))) {
    return null;
  }
  if (value.role === "controller") {
    if (typeof value.controllerId !== "string" ||
        !/^[A-Za-z0-9_-]{22}$/.test(value.controllerId)) return null;
  } else if (value.controllerId !== undefined) {
    return null;
  }
  return {
    role: value.role,
    ...(value.role === "controller" ? {controllerId: value.controllerId} : {}),
    messages: value.messages!, windowStartedAt: value.windowStartedAt!,
    ...(value.lifetimeMessages === undefined
      ? {} : {lifetimeMessages: value.lifetimeMessages}),
  };
}

const SIGNAL_WINDOW_MS = 10_000;
const SIGNAL_WINDOW_MESSAGES = 120;
const SIGNAL_LIFETIME_MESSAGES = 512;

function decodeNativeBootstrap(value: string): Uint8Array | null {
  if (value.length < 22 || value.length > 4096 ||
      !/^[A-Za-z0-9_-]+$/.test(value)) return null;
  try {
    const binary = atob(value.replace(/-/g, "+").replace(/_/g, "/") +
      "===".slice((value.length + 3) % 4));
    return Uint8Array.from(binary, character => character.charCodeAt(0));
  } catch { return null; }
}

function fallbackCode(): string {
  return String(crypto.getRandomValues(new Uint32Array(1))[0]! % 1_000_000)
    .padStart(6, "0");
}

export function admitSignalMessage(attachment: SocketAttachment,
                                   now: number): boolean {
  if (now - attachment.windowStartedAt >= SIGNAL_WINDOW_MS) {
    attachment.messages = 0;
    attachment.windowStartedAt = now;
  }
  attachment.messages += 1;
  attachment.lifetimeMessages = (attachment.lifetimeMessages || 0) + 1;
  return attachment.messages <= SIGNAL_WINDOW_MESSAGES &&
    attachment.lifetimeMessages <= SIGNAL_LIFETIME_MESSAGES;
}

function publicRoom(room: StoredRoom): Record<string, unknown> {
  return {
    type: "room_state", transitionId: room.transitionId,
    inviteExpiresAt: room.inviteExpiresAt, inviteGeneration: room.inviteGeneration,
    phase: room.phase,
    controllers: room.controllers.filter(item => item.phase !== "closed").map(item => ({
      controllerId: item.id, name: item.name,
      controllerPublicKey: item.controllerPublicKey,
      phase: item.phase, seat: item.seat, leaseGeneration: item.leaseGeneration,
      connectionSequence: item.connectionSequence,
    })),
  };
}

export class PartyRoom extends DurableObject<Env> {
  private nativeCommandTail: Promise<void> = Promise.resolve();

  private async room(): Promise<StoredRoom | undefined> {
    type LegacyRoom = Omit<StoredRoom, "version" | "fallbackCodeDigest"> & {
      version: 1;
      fallbackCode: string;
    };
    const value = await this.ctx.storage.get<StoredRoom | LegacyRoom>("room");
    if (!value || value.version === 2) return value;
    const {fallbackCode, version: _version, ...rest} = value;
    const migrated: StoredRoom = {...rest, version: 2,
      fallbackCodeDigest: await digest(this.env, "party-code", fallbackCode)};
    await this.ctx.storage.put("room", migrated);
    return migrated;
  }

  private async save(room: StoredRoom): Promise<void> {
    await this.ctx.storage.put("room", room);
  }

  private async authenticatedHost(request: Request, room: StoredRoom): Promise<boolean> {
    const supplied = request.headers.get("x-party-host-digest") || "";
    return constantTimeEqual(supplied, room.hostCredentialDigest);
  }

  override fetch(request: Request): Promise<Response> {
    /* Approval, rotation, reconnect and close are one room transition each.
     * Await points must not let a second request observe and overwrite the
     * same stored predecessor. This is also the pattern used by the budget DO. */
    return this.ctx.blockConcurrencyWhile(() => this.fetchSerialized(request));
  }

  private async fetchSerialized(request: Request): Promise<Response> {
    const rejected = rejectUnsupportedInternalApi(request);
    if (rejected) return rejected;
    const url = new URL(request.url);
    if (request.method !== "POST" && url.pathname !== "/connect") {
      return json({error: "method_not_allowed"}, 405);
    }
    if (url.pathname === "/initialize") {
      const existing = await this.room();
      if (existing) return json({error: "already_initialized"}, 409);
      const input = await readJson<CreateRoomInput>(request);
      const room: StoredRoom = {
        version: 2, ...(input.roomId ? {roomId: input.roomId} : {}),
        phase: "open", createdAt: input.now,
        expiresAt: input.now + LIMITS.roomTtlMs,
        inviteExpiresAt: input.now + LIMITS.inviteTtlMs,
        inviteDigest: input.inviteDigest, inviteGeneration: 1,
        hostCredentialDigest: input.hostCredentialDigest,
        hostPublicKey: input.hostPublicKey,
        fallbackCodeDigest: input.fallbackCodeDigest, transitionId: 1,
        nextLeaseGeneration: 1, controllers: [], closedReason: null,
      };
      await this.save(room);
      await this.ctx.storage.setAlarm(room.expiresAt);
      return json({ok: true, transitionId: room.transitionId,
        inviteExpiresAt: room.inviteExpiresAt});
    }
    const room = await this.room();
    if (!room) return json({error: "not_found"}, 404);
    if (Date.now() >= room.expiresAt) {
      this.closeSockets(4000, "room_expired");
      await this.ctx.storage.deleteAll();
      return json({error: "not_found"}, 404);
    }
    if (url.pathname === "/redeem") {
      const input = await readJson<RedeemInput>(request);
      const result = applyRoomCommand(room, {type: "redeem", value: input});
      if (!result.ok) return json({error: result.error}, 409);
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      return json({...result, hostPublicKey: room.hostPublicKey}, 201);
    }
    if (url.pathname === "/connect") return this.upgradeWebSocket(request, room);
    if (!(await this.authenticatedHost(request, room))) {
      return json({error: "unauthorized"}, 401);
    }
    const body = await readJson<Record<string, unknown>>(request);
    if (url.pathname === "/approve" || url.pathname === "/reject" ||
        url.pathname === "/remove") {
      if (typeof body.controllerId !== "string" || body.controllerId.length > 64) {
        return json({error: "invalid_controller"}, 400);
      }
      const result = url.pathname === "/approve"
        ? applyRoomCommand(room, {type: "approve",
            controllerId: body.controllerId, seat: Number(body.seat), now: Date.now()})
        : url.pathname === "/reject"
        ? applyRoomCommand(room, {type: "reject",
            controllerId: body.controllerId, now: Date.now()})
        : applyRoomCommand(room, {type: "remove",
            controllerId: body.controllerId, now: Date.now()});
      if (!result.ok) return json({error: result.error}, 409);
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      this.broadcastControllerState(room, body.controllerId);
      if (url.pathname === "/reject" || url.pathname === "/remove") {
        this.closeControllerSockets(body.controllerId,
          url.pathname === "/reject" ? "approval_rejected" : "seat_reclaimed");
      }
      return json(result);
    }
    if (url.pathname === "/rotate") {
      if (typeof body.inviteDigest !== "string" ||
          typeof body.fallbackCodeDigest !== "string" ||
          !Number.isInteger(body.expectedInviteGeneration)) {
        return json({error: "invalid_invite"}, 400);
      }
      const result = applyRoomCommand(room, {type: "rotate",
        inviteDigest: body.inviteDigest, fallbackCodeDigest: body.fallbackCodeDigest,
        expectedInviteGeneration: Number(body.expectedInviteGeneration), now: Date.now()});
      if (!result.ok) return json({error: result.error}, 409);
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      return json({...result, inviteGeneration: room.inviteGeneration,
        inviteExpiresAt: room.inviteExpiresAt});
    }
    if (url.pathname === "/revoke") {
      const result = applyRoomCommand(room, {type: "revoke", now: Date.now()});
      if (!result.ok) return json({error: result.error}, 409);
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      return json({...result, inviteGeneration: room.inviteGeneration});
    }
    if (url.pathname === "/close") {
      const result = applyRoomCommand(room, {type: "close", reason: "host_closed",
        now: Date.now()});
      await this.save(room);
      this.closeSockets(4000, "host_closed");
      return json(result);
    }
    return json({error: "not_found"}, 404);
  }

  private async upgradeWebSocket(request: Request, room: StoredRoom): Promise<Response> {
    if (request.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return json({error: "upgrade_required"}, 426);
    }
    const offered = (request.headers.get("sec-websocket-protocol") || "").split(",")
      .map(value => value.trim());
    const credentialProtocol = offered.find(value => value.startsWith("gb-ticket."));
    if (!credentialProtocol) return json({error: "unauthorized"}, 401);
    const suppliedDigest = credentialProtocol.slice("gb-ticket.".length);
    let attachment: SocketAttachment | null = null;
    if (constantTimeEqual(suppliedDigest, room.hostCredentialDigest)) {
      attachment = {role: "host", messages: 0, windowStartedAt: Date.now(),
        lifetimeMessages: 0};
    } else {
      const controller = room.controllers.find(item =>
        item.phase !== "closed" && constantTimeEqual(suppliedDigest, item.credentialDigest));
      if (controller) {
        if (controller.phase !== "pending") {
          const reconnected = applyRoomCommand(room, {type: "reconnect",
            controllerId: controller.id, now: Date.now()});
          if (!reconnected.ok) return json({error: reconnected.error}, 409);
          await this.save(room);
          this.broadcastHost(publicRoom(room));
        }
        attachment = {role: "controller", controllerId: controller.id,
          messages: 0, windowStartedAt: Date.now(), lifetimeMessages: 0};
      }
    }
    if (!attachment) return json({error: "unauthorized"}, 401);
    let nativeBootstrap = "";
    const encodedBootstrap = request.headers.get("x-mdkr-native-bootstrap") || "";
    if (encodedBootstrap) {
      if (attachment.role !== "host" || encodedBootstrap.length > 4096) {
        return json({error: "invalid_bootstrap"}, 400);
      }
      const decoded = decodeNativeBootstrap(encodedBootstrap);
      try {
        nativeBootstrap = decoded
          ? new TextDecoder("utf-8", {fatal: true, ignoreBOM: true}).decode(decoded) : "";
        const value = JSON.parse(nativeBootstrap) as Record<string, unknown>;
        if (value.type !== "native_bootstrap" ||
            typeof value.roomId !== "string" ||
            typeof value.hostCredential !== "string" ||
            typeof value.controllerUrl !== "string" ||
            typeof value.fallbackCode !== "string") {
          return json({error: "invalid_bootstrap"}, 400);
        }
      } catch {
        return json({error: "invalid_bootstrap"}, 400);
      }
    }
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    try {
      server.serializeAttachment(attachment);
      this.ctx.acceptWebSocket(server, [attachment.role]);
    } catch {
      closePartySocket(server, 1011, "socket_setup_failed");
      return json({error: "service_unavailable"}, 503);
    }
    if (attachment.role === "host") {
      /* Native bootstrap is generated by the Worker and exists only on this
       * internal upgrade. Raw credentials/invites are never persisted. */
      if (nativeBootstrap && !deliverPartySocketText(server, nativeBootstrap,
          "bootstrap_delivery_failed")) {
        return new Response(null, {status: 101, webSocket: client,
          headers: {"sec-websocket-protocol": "gb-control-v1"}});
      }
      deliverPartySocketText(server, JSON.stringify(publicRoom(room)),
        "state_delivery_failed");
    }
    else {
      const controller = room.controllers.find(item => item.id === attachment.controllerId);
      deliverPartySocketText(server, JSON.stringify({type: "controller_state",
        transitionId: room.transitionId,
        phase: controller?.phase || "closed", seat: controller?.seat || null,
        leaseGeneration: controller?.leaseGeneration || 0,
        connectionSequence: controller?.connectionSequence || 0}),
      "controller_state_delivery_failed");
    }
    return new Response(null, {status: 101, webSocket: client,
      headers: {"sec-websocket-protocol": "gb-control-v1"}});
  }

  override async webSocketMessage(socket: WebSocket,
                                  message: string | ArrayBuffer): Promise<void> {
    const attachment = socketAttachment(socket);
    if (!attachment) { closePartySocket(socket, 4001, "unauthorized"); return; }
    if ((typeof message === "string" && utf8Exceeds(message, LIMITS.maxSignalBytes)) ||
        (typeof message !== "string" && message.byteLength > LIMITS.maxSignalBytes)) {
      closePartySocket(socket, 4009, "message_too_large"); return;
    }
    if (typeof message !== "string") {
      closePartySocket(socket, 4003, "signaling_text_only"); return;
    }
    const now = Date.now();
    if (!admitSignalMessage(attachment, now)) {
      closePartySocket(socket, 4008, "rate_limited"); return;
    }
    try { socket.serializeAttachment(attachment); }
    catch { closePartySocket(socket, 1011, "attachment_update_failed"); return; }
    let parsed: unknown;
    try { parsed = JSON.parse(message) as unknown; }
    catch { closePartySocket(socket, 4003, "invalid_signaling"); return; }
    if (attachment.role === "host" && objectRecord(parsed) &&
        parsed.type === "host_command") {
      await this.enqueueNativeHostCommand(socket, parsed);
      return;
    }
    const value = normalizedPartySignal(parsed, attachment);
    if (!value) {
      closePartySocket(socket, 4003, "invalid_signaling"); return;
    }
    const forwarded = JSON.stringify(value);
    /* Signaling is authenticated and forwarded, never persisted. Pad-state
     * DataChannel traffic has no route through this object. */
    const targetTag = attachment.role === "host" ? "controller" : "host";
    for (const target of this.ctx.getWebSockets(targetTag)) {
      if (target === socket || target.readyState !== WebSocket.OPEN) continue;
      if (attachment.role === "host") {
        const targetAttachment = socketAttachment(target);
        if (!targetAttachment) {
          closePartySocket(target, 4001, "invalid_attachment");
          continue;
        }
        if (targetAttachment?.role !== "controller" ||
            targetAttachment.controllerId !== value.to) continue;
      }
      deliverPartySocketText(target, forwarded, "signaling_delivery_failed");
    }
  }

  override webSocketClose(): void {}
  override webSocketError(): void {}

  private async registerNativeCode(roomId: string, code: string,
                                   expiresAt: number): Promise<boolean> {
    const stub = this.env.PARTY_CODES.get(
      this.env.PARTY_CODES.idFromName("v1"));
    const response = await stub.fetch("https://code/register", internalRequest({
      method: "POST", headers: {"content-type": "application/json"},
      body: JSON.stringify({codeDigest: await digest(this.env, "party-code", code),
        roomId, expiresAt}),
    }));
    return response.ok;
  }

  private commandError(socket: WebSocket, error: string): void {
    deliverPartySocketText(socket,
      JSON.stringify({type: "host_command_result", ok: false, error}),
      "command_result_delivery_failed");
  }

  private enqueueNativeHostCommand(
      socket: WebSocket, value: Record<string, unknown>): Promise<void> {
    /* A Durable Object is single-threaded, but separate socket and HTTP events
     * can interleave at budget/directory/storage awaits. The local tail orders
     * two native launchers; the object-wide gate also excludes fetchSerialized
     * so neither transport can commit from the other's stale predecessor. */
    const run = this.nativeCommandTail.then(() =>
      this.ctx.blockConcurrencyWhile(async () => {
        try {
          await this.applyNativeHostCommand(socket, value);
        } catch {
          this.commandError(socket, "service_unavailable");
        }
      }));
    this.nativeCommandTail = run;
    return run;
  }

  private async reserveNativeCommand(units: 2 | 10,
                                     operation: "partyControl" | "partyRotate"):
      Promise<boolean> {
    /* The socket-lifetime reservation covers its bounded incoming-message
     * equivalents. Native host commands can additionally write room state or
     * probe the short-code directory, so admit that fanout per successful
     * command instead of assuming the launcher rotates only once. */
    try {
      const day = new Date().toISOString().slice(0, 10);
      const id = this.env.PARTY_BUDGETS.idFromName(day);
      const response = await this.env.PARTY_BUDGETS.get(id).fetch(
        `https://budget/admit?kind=control&units=${units}&operation=${operation}`,
        internalRequest({method: "POST"}));
      return response.ok;
    } catch {
      return false;
    }
  }

  private async applyNativeHostCommand(
      socket: WebSocket, value: Record<string, unknown>): Promise<void> {
    const room = await this.room();
    if (!room || room.phase === "closed") {
      this.commandError(socket, "not_found");
      return;
    }
    if (!validNativeHostCommandShape(value)) {
      this.commandError(socket, "invalid_command");
      return;
    }
    const action = typeof value.action === "string" ? value.action : "";
    const now = Date.now();
    if (action === "approve" || action === "reject" || action === "remove") {
      const controllerId = typeof value.controllerId === "string"
        ? value.controllerId : "";
      if (!controllerId || controllerId.length > 64) {
        this.commandError(socket, "invalid_controller"); return;
      }
      const result = action === "approve"
        ? applyRoomCommand(room, {type: "approve", controllerId,
            seat: Number(value.seat), now})
        : action === "reject"
        ? applyRoomCommand(room, {type: "reject", controllerId, now})
        : applyRoomCommand(room, {type: "remove", controllerId, now});
      if (!result.ok) { this.commandError(socket, result.error); return; }
      if (!(await this.reserveNativeCommand(2, "partyControl"))) {
        this.commandError(socket, "service_budget_safe"); return;
      }
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      this.broadcastControllerState(room, controllerId);
      if (action === "reject" || action === "remove") {
        this.closeControllerSockets(controllerId,
          action === "reject" ? "approval_rejected" : "seat_reclaimed");
      }
      return;
    }
    if (action === "revoke") {
      const result = applyRoomCommand(room, {type: "revoke", now});
      if (!result.ok) { this.commandError(socket, result.error); return; }
      if (!(await this.reserveNativeCommand(2, "partyControl"))) {
        this.commandError(socket, "service_budget_safe"); return;
      }
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      return;
    }
    if (action === "close") {
      const result = applyRoomCommand(room, {type: "close",
        reason: "host_closed", now});
      if (!result.ok) { this.commandError(socket, result.error); return; }
      if (!(await this.reserveNativeCommand(2, "partyControl"))) {
        this.commandError(socket, "service_budget_safe"); return;
      }
      await this.save(room);
      this.closeSockets(4000, "host_closed");
      return;
    }
    if (action === "rotate") {
      const expected = Number(value.expectedInviteGeneration);
      const roomBytes = room.roomId ? fromBase64Url(room.roomId) : null;
      if (!validPartyOrigin(this.env.PARTY_ORIGIN)) {
        this.commandError(socket, "service_unavailable"); return;
      }
      if (!room.roomId || !roomBytes || roomBytes.byteLength !== 16 ||
          !Number.isInteger(expected) || expected < 1) {
        this.commandError(socket, "invalid_invite_generation"); return;
      }
      if (expected !== room.inviteGeneration) {
        this.commandError(socket, "invalid_state"); return;
      }
      if (!(await this.reserveNativeCommand(10, "partyRotate"))) {
        this.commandError(socket, "service_budget_safe"); return;
      }
      for (let attempt = 0; attempt < 8; attempt++) {
        const capabilityBytes = new Uint8Array(32);
        capabilityBytes.set(roomBytes);
        capabilityBytes.set(crypto.getRandomValues(new Uint8Array(16)), 16);
        const capability = base64Url(capabilityBytes);
        const code = fallbackCode();
        const expiresAt = Math.min(now + LIMITS.inviteTtlMs, room.expiresAt);
        if (!(await this.registerNativeCode(room.roomId, code, expiresAt))) continue;
        const result = applyRoomCommand(room, {type: "rotate",
          inviteDigest: await digest(this.env, "invite", capability),
          fallbackCodeDigest: await digest(this.env, "party-code", code),
          expectedInviteGeneration: expected, now});
        if (!result.ok) { this.commandError(socket, result.error); return; }
        await this.save(room);
        deliverPartySocketText(socket,
          JSON.stringify({...publicRoom(room), fallbackCode: code,
            controllerUrl: `${this.env.PARTY_ORIGIN}/controller/#${capability}`}),
          "invite_delivery_failed");
        this.broadcastHost(publicRoom(room));
        return;
      }
      this.commandError(socket, "invite_rotation_failed");
      return;
    }
    this.commandError(socket, "invalid_command");
  }

  private broadcast(value: unknown): void {
    const message = JSON.stringify(value);
    for (const socket of this.ctx.getWebSockets()) {
      deliverPartySocketText(socket, message, "state_delivery_failed");
    }
  }

  private broadcastHost(value: unknown): void {
    const message = JSON.stringify(value);
    for (const socket of this.ctx.getWebSockets("host")) {
      deliverPartySocketText(socket, message, "state_delivery_failed");
    }
  }

  private broadcastControllerState(room: StoredRoom, controllerId: string): void {
    const controller = room.controllers.find(item => item.id === controllerId);
    const message = JSON.stringify({type: "controller_state",
      transitionId: room.transitionId, phase: controller?.phase || "closed",
      seat: controller?.seat || null,
      leaseGeneration: controller?.leaseGeneration || 0,
      connectionSequence: controller?.connectionSequence || 0});
    for (const socket of this.ctx.getWebSockets("controller")) {
      const attachment = socketAttachment(socket);
      if (!attachment) {
        closePartySocket(socket, 4001, "invalid_attachment");
        continue;
      }
      if (attachment.controllerId === controllerId) {
        deliverPartySocketText(socket, message,
          "controller_state_delivery_failed");
      }
    }
  }

  private closeControllerSockets(controllerId: string, reason: string): void {
    for (const socket of this.ctx.getWebSockets("controller")) {
      const attachment = socketAttachment(socket);
      if (!attachment) {
        closePartySocket(socket, 4001, "invalid_attachment");
        continue;
      }
      if (attachment.controllerId === controllerId) {
        closePartySocket(socket, 4000, reason);
      }
    }
  }

  private closeSockets(code: number, reason: string): void {
    for (const socket of this.ctx.getWebSockets()) {
      closePartySocket(socket, code, reason);
    }
  }

  override async alarm(): Promise<void> {
    const room = await this.room();
    if (room) {
      room.phase = "closed"; room.closedReason = "room_expired";
      for (const controller of room.controllers) controller.phase = "closed";
      this.closeSockets(4000, "room_expired");
    }
    await this.ctx.storage.deleteAll();
  }
}
