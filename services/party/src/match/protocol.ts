export const MATCH_PROTOCOL_VERSION = 1;
export const MATCH_LIMITS = Object.freeze({
  maxEndpoints: 4,
  maxSeats: 4,
  maxSeatsPerEndpoint: 2,
  receiptWindow: 8,
  controlLog: 64,
  roomTtlMs: 24 * 60 * 60_000,
  inviteTtlMs: 10 * 60_000,
  maxTransitions: 4096,
  maxCommandBytes: 4096,
  maxSocketMessageBytes: 4096,
  maxSignalMessageBytes: 64 * 1024,
  maxSignalSdpBytes: 60 * 1024,
  maxSignalIceBytes: 4 * 1024,
  maxSignalMetadataBytes: 256,
  signalWindowMs: 10_000,
  maxSignalMessagesPerWindow: 60,
  maxSignalMessagesPerSocket: 256,
});

export type MatchPhase = "lobby" | "loading" | "racing" | "results" | "closed";
export type MatchCommandType =
  | "join" | "leave" | "disconnect" | "reconnect" | "set_ready"
  | "set_vote" | "begin_loading" | "ack_loaded" | "begin_race"
  | "publish_results" | "rematch" | "transfer_leader" | "close"
  | "set_character" | "set_vehicle" | "cancel_loading";
export type MatchError =
  | "ok" | "protocol" | "stale_revision" | "stale_command"
  | "command_conflict" | "invalid_state" | "unauthorized" | "not_found"
  | "already_joined" | "incompatible" | "capacity" | "not_ready"
  | "disconnected" | "selection_conflict" | "illegal_vehicle";

export interface MatchCompatibilityV1 {
  protocolVersion: number;
  buildId: number[];
  gameplayDigest: number[];
  romRevision: number;
  cadenceHz: number;
}

export interface MatchMember {
  endpointId: string;
  lastCommandId: string;
  lastCommandFingerprint: string;
  seatCount: number;
  connected: boolean;
  ready: boolean;
  loaded: boolean;
}

export interface MatchSeat {
  endpointId: string;
  selectionRevision: number;
  voteTrack: number | null;
  localIndex: number;
  characterId: number | null;
  vehicleId: number | null;
}

export interface MatchReceipt {
  actorEndpointId: string;
  commandId: string;
  fingerprint: string;
}

export interface MatchLobbyV1 {
  protocolVersion: 1;
  revision: number;
  matchEpoch: number;
  leaderGeneration: number;
  roomId: string;
  leaderEndpointId: string;
  phase: MatchPhase;
  compatibility: MatchCompatibilityV1;
  members: MatchMember[];
  seats: MatchSeat[];
  receipts: MatchReceipt[];
  nextReceipt: number;
  selectedTrack: number | null;
  selectedVehicleMask: number;
}

export interface MatchCommandV1 {
  protocolVersion: number;
  expectedRevision: number;
  commandId: string;
  actorEndpointId: string;
  type: MatchCommandType;
  value: number;
  targetEndpointId: string;
  compatibility: MatchCompatibilityV1;
}

/** Exact public command body. Actor identity and compatibility are injected by
 * MatchRoom from authenticated room authority; a browser cannot supply either
 * field. Keeping this validator beside the canonical protocol lets the Worker
 * reject ambiguous media/schema before proxying while the Durable Object still
 * revalidates its own trust boundary. */
export interface MatchCommandRequestV1 {
  protocolVersion: number;
  expectedRevision: number;
  commandId: string;
  type: MatchCommandType;
  value: number;
  targetEndpointId: string;
}

export interface MatchStep {
  accepted: boolean;
  duplicate: boolean;
  leaderChanged: boolean;
  error: MatchError;
  revision: number;
  matchEpoch: number;
  leaderEndpointId: string;
  selectedTrack: number | null;
  selectedVehicleMask: number;
}

export interface MatchCredential {
  endpointId: string;
  digest: string;
  issuedAt: number;
}

export interface StoredMatchRoomV1 {
  schemaVersion: 1;
  createdAt: number;
  expiresAt: number;
  inviteExpiresAt: number;
  inviteGeneration: number;
  inviteDigest: string;
  fallbackCodeDigest: string;
  lobby: MatchLobbyV1;
  credentials: MatchCredential[];
  controlLog: MatchStep[];
  closedReason: "room_expired" | "host_closed" | null;
}

const U64_MAX = (1n << 64n) - 1n;

export function parseU64(value: unknown, allowZero = false): bigint | null {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]{0,19})$/.test(value)) return null;
  try {
    const parsed = BigInt(value);
    return parsed <= U64_MAX && (allowZero || parsed !== 0n) ? parsed : null;
  } catch { return null; }
}

function bytes(value: unknown, count: number): value is number[] {
  return Array.isArray(value) && value.length === count &&
    value.every(item => Number.isInteger(item) && item >= 0 && item <= 255);
}

export function validCompatibility(value: unknown): value is MatchCompatibilityV1 {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const keys = Object.keys(value).sort();
  const expected = ["buildId", "cadenceHz", "gameplayDigest",
    "protocolVersion", "romRevision"];
  if (keys.length !== expected.length ||
      !keys.every((key, index) => key === expected[index])) return false;
  const item = value as Partial<MatchCompatibilityV1>;
  return item.protocolVersion === MATCH_PROTOCOL_VERSION &&
    bytes(item.buildId, 16) && bytes(item.gameplayDigest, 32) &&
    Number.isInteger(item.romRevision) &&
    (item.romRevision === 1 || item.romRevision === 2) &&
    Number.isInteger(item.cadenceHz) &&
    (item.cadenceHz === 25 || item.cadenceHz === 30);
}

export function blankCompatibility(): MatchCompatibilityV1 {
  return {protocolVersion: 0, buildId: Array(16).fill(0),
    gameplayDigest: Array(32).fill(0), romRevision: 0, cadenceHz: 0};
}

export function inviteRemainingMs(inviteExpiresAt: unknown,
                                  now: unknown): number | null {
  if (!Number.isSafeInteger(inviteExpiresAt) ||
      !Number.isSafeInteger(now) || Number(now) < 0 ||
      Number(inviteExpiresAt) <= Number(now)) return null;
  return Math.min(MATCH_LIMITS.inviteTtlMs,
    Number(inviteExpiresAt) - Number(now));
}

export const MATCH_COMMAND_TYPES = new Set<MatchCommandType>([
  "join", "leave", "disconnect", "reconnect", "set_ready", "set_vote",
  "begin_loading", "ack_loaded", "begin_race", "publish_results", "rematch",
  "transfer_leader", "close", "set_character", "set_vehicle", "cancel_loading",
]);

export function validMatchCommandRequest(value: unknown):
    value is MatchCommandRequestV1 {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const item = value as Record<string, unknown>;
  const keys = Object.keys(item).sort();
  const expected = ["commandId", "expectedRevision", "protocolVersion",
    "targetEndpointId", "type", "value"];
  return keys.length === expected.length &&
    keys.every((key, index) => key === expected[index]) &&
    item.protocolVersion === MATCH_PROTOCOL_VERSION &&
    Number.isInteger(item.expectedRevision) && Number(item.expectedRevision) >= 1 &&
    Number(item.expectedRevision) <= 0xffff_ffff &&
    parseU64(item.commandId) !== null && typeof item.type === "string" &&
    MATCH_COMMAND_TYPES.has(item.type as MatchCommandType) &&
    Number.isInteger(item.value) && Number(item.value) >= 0 &&
    Number(item.value) <= 0xffff_ffff &&
    parseU64(item.targetEndpointId, true) !== null;
}

function validStoredStep(value: MatchStep): boolean {
  return value.accepted === true && value.duplicate === false &&
    typeof value.leaderChanged === "boolean" && value.error === "ok" &&
    Number.isInteger(value.revision) && value.revision >= 1 &&
    value.revision <= 0xffff_ffff && Number.isInteger(value.matchEpoch) &&
    value.matchEpoch >= 0 && value.matchEpoch <= 0xffff_ffff &&
    parseU64(value.leaderEndpointId) !== null &&
    (value.selectedTrack === null || (Number.isInteger(value.selectedTrack) &&
      value.selectedTrack >= 0 && value.selectedTrack <= 255)) &&
    Number.isInteger(value.selectedVehicleMask) && value.selectedVehicleMask >= 0 &&
    value.selectedVehicleMask <= 7;
}

export function validMatchControlLog(value: unknown, lobbyRevision: number,
                                     matchEpoch: number): value is MatchStep[] {
  if (!Array.isArray(value) || value.length > MATCH_LIMITS.controlLog) return false;
  const items = value as MatchStep[];
  if (items.some((item, index) => !item || !validStoredStep(item) ||
      item.revision > lobbyRevision || item.matchEpoch > matchEpoch ||
      (index > 0 && item.revision !== items[index - 1]!.revision + 1))) return false;
  return items.length === 0 || items[items.length - 1]!.revision === lobbyRevision;
}
