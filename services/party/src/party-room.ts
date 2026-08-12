import {DurableObject} from "cloudflare:workers";
import {applyRoomCommand} from "./room-model";
import {base64Url, constantTimeEqual, digest, fromBase64Url, json,
  readJson, utf8Exceeds} from "./security";
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

  override async fetch(request: Request): Promise<Response> {
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
      return json({ok: true, transitionId: room.transitionId});
    }
    const room = await this.room();
    if (!room) return json({error: "not_found"}, 404);
    if (Date.now() >= room.expiresAt) {
      for (const socket of this.ctx.getWebSockets()) socket.close(4000, "room_expired");
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
    if (url.pathname === "/approve" || url.pathname === "/reject") {
      if (typeof body.controllerId !== "string" || body.controllerId.length > 64) {
        return json({error: "invalid_controller"}, 400);
      }
      const result = url.pathname === "/approve"
        ? applyRoomCommand(room, {type: "approve",
            controllerId: body.controllerId, seat: Number(body.seat), now: Date.now()})
        : applyRoomCommand(room, {type: "reject",
            controllerId: body.controllerId, now: Date.now()});
      if (!result.ok) return json({error: result.error}, 409);
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      this.broadcastControllerState(room, body.controllerId);
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
      return json(result);
    }
    if (url.pathname === "/revoke") {
      const result = applyRoomCommand(room, {type: "revoke", now: Date.now()});
      if (!result.ok) return json({error: result.error}, 409);
      await this.save(room);
      this.broadcastHost(publicRoom(room));
      return json(result);
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
    server.serializeAttachment(attachment);
    this.ctx.acceptWebSocket(server, [attachment.role]);
    if (attachment.role === "host") {
      /* Native bootstrap is generated by the Worker and exists only on this
       * internal upgrade. Raw credentials/invites are never persisted. */
      if (nativeBootstrap) server.send(nativeBootstrap);
      server.send(JSON.stringify(publicRoom(room)));
    }
    else {
      const controller = room.controllers.find(item => item.id === attachment.controllerId);
      server.send(JSON.stringify({type: "controller_state", transitionId: room.transitionId,
        phase: controller?.phase || "closed", seat: controller?.seat || null,
        leaseGeneration: controller?.leaseGeneration || 0,
        connectionSequence: controller?.connectionSequence || 0}));
    }
    return new Response(null, {status: 101, webSocket: client,
      headers: {"sec-websocket-protocol": "gb-control-v1"}});
  }

  override async webSocketMessage(socket: WebSocket,
                                  message: string | ArrayBuffer): Promise<void> {
    const attachment = socket.deserializeAttachment() as SocketAttachment | null;
    if (!attachment) { socket.close(4001, "unauthorized"); return; }
    if ((typeof message === "string" && utf8Exceeds(message, LIMITS.maxSignalBytes)) ||
        (typeof message !== "string" && message.byteLength > LIMITS.maxSignalBytes)) {
      socket.close(4009, "message_too_large"); return;
    }
    if (typeof message !== "string") { socket.close(4003, "signaling_text_only"); return; }
    const now = Date.now();
    if (!admitSignalMessage(attachment, now)) {
      socket.close(4008, "rate_limited"); return;
    }
    socket.serializeAttachment(attachment);
    let value: Record<string, unknown>;
    try { value = JSON.parse(message) as Record<string, unknown>; }
    catch { socket.close(4003, "invalid_signaling"); return; }
    const controllerTypes = new Set(["controller_hello", "webrtc_answer", "webrtc_ice"]);
    const hostTypes = new Set(["webrtc_offer", "webrtc_ice", "host_command"]);
    if (typeof value.type !== "string" ||
        !(attachment.role === "host" ? hostTypes : controllerTypes).has(value.type)) {
      socket.close(4003, "invalid_signaling"); return;
    }
    if (attachment.role === "host") {
      if (value.type === "host_command") {
        await this.enqueueNativeHostCommand(socket, value);
        return;
      }
      if (typeof value.to !== "string" || value.to.length > 64) {
        socket.close(4003, "invalid_target"); return;
      }
    } else {
      /* Identity comes from the authenticated socket attachment, never from
       * controller JSON. A phone credential cannot answer or send ICE as a
       * different approved controller. */
      value.controllerId = attachment.controllerId;
    }
    const forwarded = JSON.stringify(value);
    /* Signaling is authenticated and forwarded, never persisted. Pad-state
     * DataChannel traffic has no route through this object. */
    const targetTag = attachment.role === "host" ? "controller" : "host";
    for (const target of this.ctx.getWebSockets(targetTag)) {
      if (target !== socket && target.readyState === WebSocket.OPEN) target.send(forwarded);
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
    if (socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({type: "host_command_result", ok: false, error}));
    }
  }

  private enqueueNativeHostCommand(
      socket: WebSocket, value: Record<string, unknown>): Promise<void> {
    /* A Durable Object is single-threaded, but separate socket events can
     * interleave while a command awaits budget/directory I/O. Keep every
     * authenticated host mutation in one actor-local order so two launcher
     * sockets cannot both commit from the same transition generation. */
    const run = this.nativeCommandTail.then(async () => {
      try {
        await this.applyNativeHostCommand(socket, value);
      } catch {
        this.commandError(socket, "service_unavailable");
      }
    });
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
        if (socket.readyState === WebSocket.OPEN) {
          socket.send(JSON.stringify({...publicRoom(room), fallbackCode: code,
            controllerUrl: `${this.env.PARTY_ORIGIN}/controller/#${capability}`}));
        }
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
      if (socket.readyState === WebSocket.OPEN) socket.send(message);
    }
  }

  private broadcastHost(value: unknown): void {
    const message = JSON.stringify(value);
    for (const socket of this.ctx.getWebSockets("host")) {
      if (socket.readyState === WebSocket.OPEN) socket.send(message);
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
      const attachment = socket.deserializeAttachment() as SocketAttachment | null;
      if (attachment?.controllerId === controllerId && socket.readyState === WebSocket.OPEN) {
        socket.send(message);
      }
    }
  }

  private closeSockets(code: number, reason: string): void {
    for (const socket of this.ctx.getWebSockets()) socket.close(code, reason);
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
