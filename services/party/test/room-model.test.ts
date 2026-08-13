import {describe, expect, it} from "vitest";
import {applyRoomCommand} from "../src/room-model";
import {admitSignalMessage} from "../src/party-room";
import {LIMITS, partyInviteRemainingMs, type StoredRoom} from "../src/types";

function room(): StoredRoom {
  return {version: 2, phase: "open", createdAt: 0,
    expiresAt: LIMITS.roomTtlMs, inviteExpiresAt: LIMITS.inviteTtlMs,
    inviteDigest: "digest", inviteGeneration: 1,
    hostCredentialDigest: "host", hostPublicKey: "H".repeat(87),
    fallbackCodeDigest: "code-digest", transitionId: 1,
    nextLeaseGeneration: 1, controllers: [], closedReason: null};
}

function redeem(target: StoredRoom, id: string, now = 1) {
  return applyRoomCommand(target, {type: "redeem", value: {
    inviteDigest: "digest", fallbackCodeDigest: "", controllerId: id, credentialDigest: `credential-${id}`,
    name: id, controllerPublicKey: "C".repeat(87), protocol: 1, now}});
}

describe("Party room state model", () => {
  it("bounds advertised controller-invite lifetime by current room authority", () => {
    expect(partyInviteRemainingMs(1_000, 900)).toBe(100);
    expect(partyInviteRemainingMs(LIMITS.inviteTtlMs + 10, 0))
      .toBe(LIMITS.inviteTtlMs);
    expect(partyInviteRemainingMs(900, 900)).toBeNull();
    expect(partyInviteRemainingMs(899, 900)).toBeNull();
    expect(partyInviteRemainingMs(Number.MAX_SAFE_INTEGER + 1, 0)).toBeNull();
  });

  it("bounds signaling by both burst window and socket lifetime", () => {
    const burst = {role: "host" as const, messages: 0,
      windowStartedAt: 0, lifetimeMessages: 0};
    for (let index = 0; index < 120; index++) {
      expect(admitSignalMessage(burst, 0)).toBe(true);
    }
    expect(admitSignalMessage(burst, 0)).toBe(false);

    const lifetime = {role: "controller" as const, controllerId: "phone",
      messages: 0, windowStartedAt: 0, lifetimeMessages: 0};
    for (let index = 0; index < 512; index++) {
      const window = Math.floor(index / 120);
      expect(admitSignalMessage(lifetime, window * 10_000)).toBe(true);
    }
    expect(admitSignalMessage(lifetime, 50_000)).toBe(false);
  });

  it("redeems, approves unique seats, and generation-checks by construction", () => {
    const value = room();
    expect(redeem(value, "one").ok).toBe(true);
    expect(redeem(value, "two").ok).toBe(true);
    const first = applyRoomCommand(value, {type: "approve", controllerId: "one", seat: 3, now: 2});
    const second = applyRoomCommand(value, {type: "approve", controllerId: "two", seat: 2, now: 2});
    expect(first).toMatchObject({ok: true, seat: 3, leaseGeneration: 1});
    expect(second).toMatchObject({ok: true, seat: 2, leaseGeneration: 2});
    expect(value.controllers.map(item => item.seat)).toEqual([3, 2]);
  });

  it("fails closed for an invalid or already leased host-selected seat", () => {
    const value = room();
    redeem(value, "one");
    redeem(value, "two");
    expect(applyRoomCommand(value,
      {type: "approve", controllerId: "one", seat: 0, now: 2}))
      .toEqual({ok: false, error: "invalid_state"});
    expect(applyRoomCommand(value,
      {type: "approve", controllerId: "one", seat: 4, now: 2}).ok).toBe(true);
    expect(applyRoomCommand(value,
      {type: "approve", controllerId: "two", seat: 4, now: 2}))
      .toEqual({ok: false, error: "room_full"});
    expect(value.controllers[1]!.phase).toBe("pending");
  });

  it("rejects replay after invite rotation", () => {
    const value = room();
    expect(applyRoomCommand(value, {type: "rotate", inviteDigest: "new",
      fallbackCodeDigest: "new-code", expectedInviteGeneration: 1, now: 10}).ok).toBe(true);
    expect(redeem(value, "replay", 11)).toEqual({ok: false, error: "invite_rotated"});
  });

  it("rejects a late rotate after dismissal without reviving an invite", () => {
    const value = room();
    expect(applyRoomCommand(value, {type: "revoke", now: 2}).ok).toBe(true);
    expect(applyRoomCommand(value, {type: "rotate", inviteDigest: "late",
      fallbackCodeDigest: "late-code", expectedInviteGeneration: 1, now: 3}))
      .toEqual({ok: false, error: "invalid_state"});
    expect(value).toMatchObject({inviteDigest: "", fallbackCodeDigest: "",
      inviteGeneration: 2, inviteExpiresAt: 2});
  });

  it("revokes the visible invite without disconnecting approved controllers", () => {
    const value = room();
    redeem(value, "phone");
    applyRoomCommand(value,
      {type: "approve", controllerId: "phone", seat: 2, now: 2});
    expect(applyRoomCommand(value, {type: "revoke", now: 3}).ok).toBe(true);
    expect(value).toMatchObject({phase: "open", inviteDigest: "",
      fallbackCodeDigest: "", inviteExpiresAt: 3});
    expect(value.controllers[0]).toMatchObject({phase: "leased", seat: 2});
    expect(redeem(value, "late", 4)).toEqual({ok: false, error: "invite_expired"});
  });

  it("caps pending devices before mutation", () => {
    const value = room();
    for (let index = 0; index < LIMITS.maxPending; index++) {
      expect(redeem(value, String(index)).ok).toBe(true);
    }
    const transitions = value.transitionId;
    expect(redeem(value, "overflow")).toEqual({ok: false, error: "pending_full"});
    expect(value.transitionId).toBe(transitions);
    expect(value.controllers).toHaveLength(LIMITS.maxPending);
  });

  it("advances the connection epoch without changing the seat lease", () => {
    const value = room();
    redeem(value, "phone");
    applyRoomCommand(value, {type: "approve", controllerId: "phone", seat: 1, now: 2});
    expect(applyRoomCommand(value, {type: "reconnect", controllerId: "phone", now: 3}))
      .toMatchObject({ok: true, seat: 1, leaseGeneration: 1,
        connectionSequence: 2});
    expect(value.controllers[0]).toMatchObject({phase: "connected", seat: 1,
      leaseGeneration: 1, connectionSequence: 2});
  });

  it("removes an approved controller and releases its seat atomically", () => {
    const value = room();
    redeem(value, "phone");
    applyRoomCommand(value, {type: "approve", controllerId: "phone", seat: 2, now: 2});
    expect(applyRoomCommand(value,
      {type: "remove", controllerId: "phone", now: 3})).toMatchObject({ok: true});
    expect(value.controllers[0]).toMatchObject({phase: "closed", seat: null});
    expect(applyRoomCommand(value,
      {type: "remove", controllerId: "phone", now: 4}))
      .toEqual({ok: false, error: "not_found"});
  });

  it("closes every controller and cannot be revived", () => {
    const value = room();
    redeem(value, "one");
    applyRoomCommand(value, {type: "close", reason: "host_closed", now: 2});
    expect(value.controllers[0]!.phase).toBe("closed");
    expect(redeem(value, "late", 3)).toEqual({ok: false, error: "host_closed"});
  });
});
