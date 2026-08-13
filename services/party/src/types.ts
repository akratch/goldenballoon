export const LIMITS = Object.freeze({
  inviteTtlMs: 2 * 60_000,
  roomTtlMs: 24 * 60 * 60_000,
  maxPending: 8,
  maxSeats: 4,
  maxJsonBytes: 16 * 1024,
  maxSignalBytes: 64 * 1024,
  maxNameCodePoints: 24,
  maxTransitions: 4096,
});

export function partyInviteRemainingMs(inviteExpiresAt: unknown,
                                       now: unknown): number | null {
  if (!Number.isSafeInteger(inviteExpiresAt) || !Number.isSafeInteger(now) ||
      Number(now) < 0 || Number(inviteExpiresAt) <= Number(now)) return null;
  return Math.min(LIMITS.inviteTtlMs,
    Number(inviteExpiresAt) - Number(now));
}

/* Phone Party owns controller admission/leases, not gameplay progression.
 * Starting a local race revokes the displayed invite but leaves this room
 * open, keeping the launcher/service boundary out of game state. */
export type RoomPhase = "open" | "closed";
export type ControllerPhase =
  "pending" | "approved" | "leased" | "connected" | "closed";
export type CloseReason =
  | "invite_expired" | "invite_rotated" | "room_full" | "pending_full"
  | "approval_rejected" | "host_closed" | "room_expired"
  | "protocol_update_required" | "duplicate_controller" | "seat_reclaimed"
  | "rate_limited" | "service_budget_safe" | "transport_lost";

export interface Env {
  PARTY_ROOMS: DurableObjectNamespace<import("./party-room").PartyRoom>;
  PARTY_BUDGETS: DurableObjectNamespace<import("./party-budget").PartyBudget>;
  PARTY_CODES: DurableObjectNamespace<import("./party-code-directory").PartyCodeDirectory>;
  MATCH_ROOMS: DurableObjectNamespace<import("./match/match-room").MatchRoom>;
  PARTY_HMAC_KEY: string;
  PARTY_ORIGIN: string;
  MAX_ADMISSIONS_PER_DAY: string;
  CONTROL_RESERVE_PER_DAY: string;
  OPS_READ_TOKEN?: string;
}

export interface StoredController {
  id: string;
  credentialDigest: string;
  phase: ControllerPhase;
  seat: number | null;
  leaseGeneration: number;
  connectionSequence: number;
  name: string;
  controllerPublicKey: string;
  createdAt: number;
}

export interface StoredRoom {
  version: 2;
  /* Additive v2 field. Older browser-created rooms may omit it; only the
   * native authenticated socket needs the name to rotate its invite in-DO. */
  roomId?: string;
  phase: RoomPhase;
  createdAt: number;
  expiresAt: number;
  inviteExpiresAt: number;
  inviteDigest: string;
  inviteGeneration: number;
  hostCredentialDigest: string;
  hostPublicKey: string;
  fallbackCodeDigest: string;
  transitionId: number;
  nextLeaseGeneration: number;
  controllers: StoredController[];
  closedReason: CloseReason | null;
}

export interface CreateRoomInput {
  roomId?: string;
  inviteDigest: string;
  hostCredentialDigest: string;
  hostPublicKey: string;
  fallbackCodeDigest: string;
  now: number;
}

export interface RedeemInput {
  inviteDigest: string;
  fallbackCodeDigest: string;
  controllerId: string;
  credentialDigest: string;
  name: string;
  controllerPublicKey: string;
  protocol: number;
  now: number;
}
