import {LIMITS, type CloseReason, type RedeemInput, type StoredRoom} from "./types";
import {constantTimeEqual} from "./security";

export type RoomCommand =
  | {type: "redeem"; value: RedeemInput}
  | {type: "approve"; controllerId: string; seat: number; now: number}
  | {type: "reject"; controllerId: string; now: number}
  | {type: "remove"; controllerId: string; now: number}
  | {type: "reconnect"; controllerId: string; now: number}
  | {type: "rotate"; inviteDigest: string; fallbackCodeDigest: string;
      expectedInviteGeneration: number; now: number}
  | {type: "revoke"; now: number}
  | {type: "close"; reason: CloseReason; now: number};

export type RoomResult =
  | {ok: true; transitionId: number; controllerId?: string; seat?: number;
      leaseGeneration?: number; connectionSequence?: number}
  | {ok: false; error: CloseReason | "invalid_state" | "not_found"};

function closed(room: StoredRoom, now: number): CloseReason | null {
  if (room.phase === "closed") return room.closedReason || "host_closed";
  if (now >= room.expiresAt) return "room_expired";
  return null;
}

function advance(room: StoredRoom): number {
  room.transitionId++;
  if (room.transitionId > LIMITS.maxTransitions) {
    room.phase = "closed";
    room.closedReason = "rate_limited";
  }
  return room.transitionId;
}

export function applyRoomCommand(room: StoredRoom, command: RoomCommand): RoomResult {
  const terminal = closed(room, "now" in command ? command.now : 0);
  if (terminal) return {ok: false, error: terminal};
  if (command.type === "redeem") {
    const input = command.value;
    // Pairing protocol 2 = SAS v2 (the phrase binds both DTLS fingerprints).
    // A protocol-1 page derives words this room's hosts can never match, so
    // it is refused here rather than paired into a phrase that cannot verify.
    if (input.protocol !== 2) return {ok: false, error: "protocol_update_required"};
    if (input.now >= room.inviteExpiresAt) return {ok: false, error: "invite_expired"};
    const inviteMatches = input.inviteDigest.length > 0 &&
      constantTimeEqual(input.inviteDigest, room.inviteDigest);
    const codeMatches = input.fallbackCodeDigest.length > 0 &&
      constantTimeEqual(input.fallbackCodeDigest, room.fallbackCodeDigest);
    if (!inviteMatches && !codeMatches) {
      return {ok: false, error: "invite_rotated"};
    }
    if (room.controllers.filter(item => item.phase === "pending").length >= LIMITS.maxPending) {
      return {ok: false, error: "pending_full"};
    }
    if (room.controllers.some(item => item.id === input.controllerId && item.phase !== "closed")) {
      return {ok: false, error: "duplicate_controller"};
    }
    room.controllers.push({
      id: input.controllerId, credentialDigest: input.credentialDigest,
      phase: "pending", seat: null, leaseGeneration: 0,
      connectionSequence: 1, name: input.name,
      controllerPublicKey: input.controllerPublicKey,
      createdAt: input.now,
    });
    return {ok: true, transitionId: advance(room), controllerId: input.controllerId};
  }
  if (command.type === "approve") {
    const controller = room.controllers.find(item =>
      item.id === command.controllerId && item.phase === "pending");
    if (!controller) return {ok: false, error: "not_found"};
    const occupied = new Set(room.controllers
      .filter(item => item.seat !== null && item.phase !== "closed")
      .map(item => item.seat));
    const seat = command.seat;
    if (!Number.isInteger(seat) || seat < 1 || seat > LIMITS.maxSeats) {
      return {ok: false, error: "invalid_state"};
    }
    if (occupied.has(seat)) return {ok: false, error: "room_full"};
    controller.phase = "leased";
    controller.seat = seat;
    controller.leaseGeneration = room.nextLeaseGeneration++;
    return {ok: true, transitionId: advance(room), controllerId: controller.id,
      seat, leaseGeneration: controller.leaseGeneration,
      connectionSequence: controller.connectionSequence};
  }
  if (command.type === "reject") {
    const controller = room.controllers.find(item =>
      item.id === command.controllerId && item.phase === "pending");
    if (!controller) return {ok: false, error: "not_found"};
    controller.phase = "closed";
    return {ok: true, transitionId: advance(room), controllerId: controller.id};
  }
  if (command.type === "remove") {
    const controller = room.controllers.find(item =>
      item.id === command.controllerId && item.phase !== "pending" &&
      item.phase !== "closed");
    if (!controller) return {ok: false, error: "not_found"};
    controller.phase = "closed";
    controller.seat = null;
    return {ok: true, transitionId: advance(room), controllerId: controller.id};
  }
  if (command.type === "reconnect") {
    const controller = room.controllers.find(item =>
      item.id === command.controllerId &&
      item.seat !== null &&
      ["approved", "leased", "connected"].includes(item.phase));
    if (!controller) return {ok: false, error: "not_found"};
    controller.phase = "connected";
    controller.connectionSequence = (controller.connectionSequence + 1) >>> 0;
    if (controller.connectionSequence === 0) controller.connectionSequence = 1;
    return {ok: true, transitionId: advance(room), controllerId: controller.id,
      seat: controller.seat!,
      leaseGeneration: controller.leaseGeneration,
      connectionSequence: controller.connectionSequence};
  }
  if (command.type === "rotate") {
    if (!Number.isInteger(command.expectedInviteGeneration) ||
        command.expectedInviteGeneration !== room.inviteGeneration) {
      return {ok: false, error: "invalid_state"};
    }
    room.inviteDigest = command.inviteDigest;
    room.fallbackCodeDigest = command.fallbackCodeDigest;
    room.inviteGeneration++;
    room.inviteExpiresAt = Math.min(command.now + LIMITS.inviteTtlMs, room.expiresAt);
    return {ok: true, transitionId: advance(room)};
  }
  if (command.type === "revoke") {
    room.inviteDigest = "";
    room.fallbackCodeDigest = "";
    room.inviteGeneration++;
    room.inviteExpiresAt = command.now;
    return {ok: true, transitionId: advance(room)};
  }
  room.phase = "closed";
  room.closedReason = command.reason;
  for (const controller of room.controllers) controller.phase = "closed";
  return {ok: true, transitionId: advance(room)};
}
