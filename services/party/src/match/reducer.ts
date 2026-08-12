import {MATCH_LIMITS, MATCH_PROTOCOL_VERSION, type MatchCommandType,
  type MatchCommandV1, type MatchCompatibilityV1, type MatchError,
  type MatchLobbyV1, type MatchMember, type MatchSeat, type MatchStep,
  parseU64, validCompatibility} from "./protocol";

const U32_MAX = 0xffff_ffff;
const PHASES = new Set(["lobby", "loading", "racing", "results", "closed"]);
const COMMAND_NUMBER: Record<MatchCommandType, number> = {
  join: 1, leave: 2, disconnect: 3, reconnect: 4, set_ready: 5,
  set_vote: 6, begin_loading: 7, ack_loaded: 8, begin_race: 9,
  publish_results: 10, rematch: 11, transfer_leader: 12, close: 13,
  set_character: 14, set_vehicle: 15, cancel_loading: 16,
};

function sameCompatibility(a: MatchCompatibilityV1, b: MatchCompatibilityV1): boolean {
  return a.protocolVersion === b.protocolVersion && a.romRevision === b.romRevision &&
    a.cadenceHz === b.cadenceHz && a.buildId.every((v, i) => v === b.buildId[i]) &&
    a.gameplayDigest.every((v, i) => v === b.gameplayDigest[i]);
}

function fnvU64(hash: bigint, value: bigint): bigint {
  for (let shift = 0n; shift < 64n; shift += 8n) {
    hash ^= (value >> shift) & 255n;
    hash = BigInt.asUintN(64, hash * 1099511628211n);
  }
  return hash;
}

export function matchCommandFingerprint(command: MatchCommandV1): string {
  let hash = 1469598103934665603n;
  hash = fnvU64(hash, BigInt(COMMAND_NUMBER[command.type]));
  hash = fnvU64(hash, BigInt(command.value >>> 0));
  hash = fnvU64(hash, parseU64(command.targetEndpointId, true) || 0n);
  hash = fnvU64(hash, BigInt(command.compatibility.protocolVersion));
  for (const byte of command.compatibility.buildId) hash = fnvU64(hash, BigInt(byte));
  for (const byte of command.compatibility.gameplayDigest) hash = fnvU64(hash, BigInt(byte));
  hash = fnvU64(hash, BigInt(command.compatibility.romRevision));
  hash = fnvU64(hash, BigInt(command.compatibility.cadenceHz));
  return hash.toString(16).padStart(16, "0");
}

function step(lobby: MatchLobbyV1, accepted: boolean, error: MatchError,
              duplicate = false, leaderChanged = false): MatchStep {
  return {accepted, duplicate, leaderChanged, error, revision: lobby.revision,
    matchEpoch: lobby.matchEpoch, leaderEndpointId: lobby.leaderEndpointId,
    selectedTrack: lobby.selectedTrack,
    selectedVehicleMask: lobby.selectedVehicleMask};
}

function member(lobby: MatchLobbyV1, endpointId: string): MatchMember | undefined {
  return lobby.members.find(item => item.endpointId === endpointId);
}

function clearRound(lobby: MatchLobbyV1): void {
  for (const item of lobby.members) { item.ready = false; item.loaded = false; }
  for (const seat of lobby.seats) seat.voteTrack = null;
  lobby.selectedTrack = null;
  lobby.selectedVehicleMask = 0;
}

function complete(lobby: MatchLobbyV1, endpointId: string): boolean {
  return lobby.seats.filter(seat => seat.endpointId === endpointId)
    .every(seat => seat.characterId !== null && seat.vehicleId !== null);
}

function allReady(lobby: MatchLobbyV1): boolean {
  return lobby.members.length >= 2 &&
    lobby.members.every(item => item.connected && item.ready);
}

function selectTrack(lobby: MatchLobbyV1): number | null {
  const counts = new Map<number, number>();
  for (const seat of lobby.seats) {
    if (seat.voteTrack !== null) counts.set(seat.voteTrack,
      (counts.get(seat.voteTrack) || 0) + 1);
  }
  const best = Math.max(0, ...counts.values());
  const candidates = [...counts].filter(([, count]) => count === best)
    .map(([track]) => track).sort((a, b) => a - b);
  if (!candidates.length) return null;
  const index = Number((BigInt(lobby.roomId) ^ BigInt(lobby.matchEpoch + 1)) %
    BigInt(candidates.length));
  return candidates[index] ?? null;
}

function addMember(lobby: MatchLobbyV1, endpointId: string, seatCount: number): boolean {
  if (!Number.isInteger(seatCount) || seatCount < 1 ||
      seatCount > MATCH_LIMITS.maxSeatsPerEndpoint ||
      lobby.members.length >= MATCH_LIMITS.maxEndpoints ||
      lobby.seats.length + seatCount > MATCH_LIMITS.maxSeats) return false;
  lobby.members.push({endpointId, lastCommandId: "0", lastCommandFingerprint: "",
    seatCount, connected: true, ready: false, loaded: false});
  for (let localIndex = 0; localIndex < seatCount; localIndex++) {
    lobby.seats.push({endpointId, selectionRevision: 0, voteTrack: null,
      localIndex, characterId: null, vehicleId: null});
  }
  return true;
}

export function createMatchLobby(roomId: string, leaderEndpointId: string,
                                 compatibility: MatchCompatibilityV1,
                                 leaderSeats: number): MatchLobbyV1 | null {
  if (!parseU64(roomId) || !parseU64(leaderEndpointId) ||
      !validCompatibility(compatibility)) return null;
  const lobby: MatchLobbyV1 = {protocolVersion: 1, revision: 1, matchEpoch: 0,
    leaderGeneration: 1, roomId, leaderEndpointId, phase: "lobby",
    compatibility: structuredClone(compatibility), members: [], seats: [],
    receipts: [], nextReceipt: 0, selectedTrack: null, selectedVehicleMask: 0};
  return addMember(lobby, leaderEndpointId, leaderSeats) ? lobby : null;
}

export function validMatchLobby(lobby: MatchLobbyV1): boolean {
  if (lobby.protocolVersion !== MATCH_PROTOCOL_VERSION || !parseU64(lobby.roomId) ||
      !parseU64(lobby.leaderEndpointId) || !validCompatibility(lobby.compatibility) ||
      !PHASES.has(lobby.phase) || !Number.isInteger(lobby.leaderGeneration) ||
      lobby.leaderGeneration < 1 || lobby.leaderGeneration > U32_MAX ||
      !Number.isInteger(lobby.revision) || lobby.revision < 1 ||
      lobby.revision > U32_MAX || !Number.isInteger(lobby.matchEpoch) ||
      lobby.matchEpoch < 0 || lobby.matchEpoch > U32_MAX ||
      lobby.members.length < 1 || lobby.members.length > MATCH_LIMITS.maxEndpoints ||
      lobby.seats.length < 1 || lobby.seats.length > MATCH_LIMITS.maxSeats ||
      !member(lobby, lobby.leaderEndpointId) ||
      (lobby.selectedTrack !== null && (!Number.isInteger(lobby.selectedTrack) ||
        lobby.selectedTrack < 0 || lobby.selectedTrack > 255)) ||
      !Number.isInteger(lobby.selectedVehicleMask) || lobby.selectedVehicleMask < 0 ||
      lobby.selectedVehicleMask > 7 ||
      lobby.receipts.length > MATCH_LIMITS.receiptWindow ||
      !Number.isInteger(lobby.nextReceipt) || lobby.nextReceipt < 0 ||
      lobby.nextReceipt >= MATCH_LIMITS.receiptWindow ||
      lobby.receipts.some(receipt => !parseU64(receipt.actorEndpointId) ||
        !parseU64(receipt.commandId) || !/^[0-9a-f]{16}$/.test(receipt.fingerprint)))
    return false;
  const ids = new Set<string>();
  const characters = new Set<number>();
  for (const item of lobby.members) {
    const localIndices = lobby.seats.filter(seat => seat.endpointId === item.endpointId)
      .map(seat => seat.localIndex);
    if (!parseU64(item.endpointId) || ids.has(item.endpointId) ||
        !Number.isInteger(item.seatCount) || item.seatCount < 1 ||
        item.seatCount > MATCH_LIMITS.maxSeatsPerEndpoint ||
        typeof item.connected !== "boolean" || typeof item.ready !== "boolean" ||
        typeof item.loaded !== "boolean" ||
        localIndices.length !== item.seatCount || new Set(localIndices).size !==
          localIndices.length || (item.ready && !complete(lobby, item.endpointId)) ||
        parseU64(item.lastCommandId, true) === null ||
        (item.lastCommandId === "0" ? item.lastCommandFingerprint !== "" :
          !/^[0-9a-f]{16}$/.test(item.lastCommandFingerprint))) return false;
    ids.add(item.endpointId);
  }
  for (const seat of lobby.seats) {
    if (!ids.has(seat.endpointId) || !Number.isInteger(seat.localIndex) || seat.localIndex < 0 ||
        seat.localIndex >= (member(lobby, seat.endpointId)?.seatCount || 0) ||
        (seat.voteTrack !== null && (!Number.isInteger(seat.voteTrack) ||
          seat.voteTrack < 0 || seat.voteTrack > 255)) ||
        !Number.isInteger(seat.selectionRevision) || seat.selectionRevision < 0 ||
        seat.selectionRevision > U32_MAX ||
        (seat.characterId !== null && (!Number.isInteger(seat.characterId) ||
          seat.characterId < 0 || seat.characterId >= 10 ||
          characters.has(seat.characterId))) ||
        (seat.vehicleId !== null && (!Number.isInteger(seat.vehicleId) ||
          seat.vehicleId < 0 || seat.vehicleId >= 3)) ||
        ((seat.characterId !== null || seat.vehicleId !== null) !==
          (seat.selectionRevision !== 0))) return false;
    if (seat.characterId !== null) characters.add(seat.characterId);
  }
  const active = lobby.phase === "loading" || lobby.phase === "racing" ||
    lobby.phase === "results";
  return (!active || (lobby.selectedTrack !== null && lobby.selectedVehicleMask > 0 &&
    lobby.matchEpoch > 0 && lobby.seats.every(seat => seat.characterId !== null &&
      seat.vehicleId !== null &&
      (lobby.selectedVehicleMask & (1 << seat.vehicleId)) !== 0))) &&
    (lobby.phase !== "lobby" ||
      (lobby.selectedTrack === null && lobby.selectedVehicleMask === 0 &&
       lobby.members.every(item => !item.loaded)));
}

function retainReceipt(lobby: MatchLobbyV1, command: MatchCommandV1,
                       fingerprint: string): void {
  const receipt = {actorEndpointId: command.actorEndpointId,
    commandId: command.commandId, fingerprint};
  if (lobby.receipts.length < MATCH_LIMITS.receiptWindow) lobby.receipts.push(receipt);
  else lobby.receipts[lobby.nextReceipt] = receipt;
  lobby.nextReceipt = (lobby.nextReceipt + 1) % MATCH_LIMITS.receiptWindow;
}

export function dispatchMatchCommand(lobby: MatchLobbyV1,
                                     command: MatchCommandV1): MatchStep {
  if (command.protocolVersion !== MATCH_PROTOCOL_VERSION) return step(lobby, false, "protocol");
  const commandId = parseU64(command.commandId);
  const target = parseU64(command.targetEndpointId, true);
  const fingerprintCompatibility = command.compatibility;
  if (!validMatchLobby(lobby) || !commandId || !parseU64(command.actorEndpointId) ||
      target === null || !Number.isInteger(command.expectedRevision) ||
      command.expectedRevision < 1 || command.expectedRevision > U32_MAX ||
      !Number.isInteger(command.value) || command.value < 0 || command.value > U32_MAX ||
      !Object.hasOwn(COMMAND_NUMBER, command.type) || !fingerprintCompatibility ||
      !Array.isArray(fingerprintCompatibility.buildId) ||
      fingerprintCompatibility.buildId.length !== 16 ||
      !Array.isArray(fingerprintCompatibility.gameplayDigest) ||
      fingerprintCompatibility.gameplayDigest.length !== 32 ||
      ![...fingerprintCompatibility.buildId,
        ...fingerprintCompatibility.gameplayDigest].every(byte =>
          Number.isInteger(byte) && byte >= 0 && byte <= 255) ||
      !Number.isInteger(fingerprintCompatibility.protocolVersion) ||
      !Number.isInteger(fingerprintCompatibility.romRevision) ||
      !Number.isInteger(fingerprintCompatibility.cadenceHz))
    return step(lobby, false, "invalid_state");
  let actor = member(lobby, command.actorEndpointId);
  const fingerprint = matchCommandFingerprint(command);
  const receipt = lobby.receipts.find(item => item.actorEndpointId === command.actorEndpointId &&
    item.commandId === command.commandId);
  if (receipt) return receipt.fingerprint === fingerprint
    ? step(lobby, true, "ok", true) : step(lobby, false, "command_conflict");
  if (actor && command.commandId === actor.lastCommandId) {
    return actor.lastCommandFingerprint === fingerprint
      ? step(lobby, true, "ok", true) : step(lobby, false, "command_conflict");
  }
  if (command.expectedRevision !== lobby.revision) return step(lobby, false, "stale_revision");
  if (actor && commandId < BigInt(actor.lastCommandId)) return step(lobby, false, "stale_command");
  if (lobby.phase === "closed") return step(lobby, false, "invalid_state");
  if (!actor && command.type !== "join") return step(lobby, false, "not_found");
  if (actor && !actor.connected && command.type !== "reconnect" && command.type !== "leave")
    return step(lobby, false, "disconnected");

  const next = structuredClone(lobby);
  actor = member(next, command.actorEndpointId);
  let leaderChanged = false;
  const reject = (error: MatchError): MatchStep => step(lobby, false, error);
  switch (command.type) {
    case "join":
      if (actor) return reject("already_joined");
      if (next.phase !== "lobby") return reject("invalid_state");
      if (!validCompatibility(command.compatibility) ||
          !sameCompatibility(next.compatibility, command.compatibility)) return reject("incompatible");
      if (!addMember(next, command.actorEndpointId, command.value)) return reject("capacity");
      actor = member(next, command.actorEndpointId);
      break;
    case "leave": {
      if ((next.phase !== "lobby" && next.phase !== "results") || next.members.length <= 1)
        return reject("invalid_state");
      next.members = next.members.filter(item => item.endpointId !== command.actorEndpointId);
      next.seats = next.seats.filter(seat => seat.endpointId !== command.actorEndpointId);
      if (next.leaderEndpointId === command.actorEndpointId) {
        const connected = next.members.filter(item => item.connected)
          .sort((a, b) => BigInt(a.endpointId) < BigInt(b.endpointId) ? -1 : 1);
        if (!connected[0]) return reject("invalid_state");
        next.leaderEndpointId = connected[0].endpointId;
        next.leaderGeneration++; leaderChanged = true;
      }
      actor = undefined;
      break;
    }
    case "disconnect": actor!.connected = false; actor!.ready = false; actor!.loaded = false; break;
    case "reconnect": actor!.connected = true; actor!.ready = false; actor!.loaded = false; break;
    case "set_ready":
      if (next.phase !== "lobby" || command.value > 1) return reject("invalid_state");
      if (command.value === 1 && !complete(next, command.actorEndpointId)) return reject("not_ready");
      actor!.ready = command.value === 1; break;
    case "set_vote": {
      const index = Number(parseU64(command.targetEndpointId, true));
      if (next.phase !== "lobby" || command.value > 255 || !Number.isInteger(index) ||
          index < 0 || index >= MATCH_LIMITS.maxSeats) return reject("invalid_state");
      const seat = next.seats[index];
      if (!seat || seat.endpointId !== command.actorEndpointId) return reject("unauthorized");
      seat.voteTrack = command.value; actor!.ready = false; break;
    }
    case "set_character":
    case "set_vehicle": {
      const index = Number(parseU64(command.targetEndpointId, true));
      if (next.phase !== "lobby" || !Number.isInteger(index) || index < 0 ||
          index >= MATCH_LIMITS.maxSeats) return reject("invalid_state");
      const seat = next.seats[index];
      if (!seat || seat.endpointId !== command.actorEndpointId) return reject("unauthorized");
      if (command.type === "set_character") {
        if (command.value >= 10) return reject("invalid_state");
        if (next.seats.some(other => other !== seat && other.characterId === command.value))
          return reject("selection_conflict");
        seat.characterId = command.value;
      } else {
        if (command.value >= 3) return reject("invalid_state");
        seat.vehicleId = command.value;
      }
      if (seat.selectionRevision === U32_MAX) return reject("invalid_state");
      seat.selectionRevision++; actor!.ready = false; break;
    }
    case "begin_loading": {
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      if (next.phase !== "lobby" || !allReady(next)) return reject("not_ready");
      const selected = selectTrack(next);
      if (selected === null) return reject("not_ready");
      if (command.value < 1 || (command.value & ~7) !== 0 ||
          next.seats.some(seat => seat.vehicleId === null ||
            (command.value & (1 << seat.vehicleId)) === 0)) return reject("illegal_vehicle");
      if (next.matchEpoch === U32_MAX) return reject("invalid_state");
      next.selectedTrack = selected; next.selectedVehicleMask = command.value;
      next.matchEpoch++; next.phase = "loading";
      for (const item of next.members) item.loaded = false;
      break;
    }
    case "ack_loaded":
      if (next.phase !== "loading") return reject("invalid_state");
      actor!.loaded = true; break;
    case "begin_race":
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      if (next.phase !== "loading" ||
          !next.members.every(item => item.connected && item.loaded)) return reject("not_ready");
      next.phase = "racing"; break;
    case "cancel_loading":
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      if (next.phase !== "loading") return reject("invalid_state");
      next.phase = "lobby"; clearRound(next); break;
    case "publish_results":
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      if (next.phase !== "racing") return reject("invalid_state");
      next.phase = "results"; break;
    case "rematch":
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      if (next.phase !== "results") return reject("invalid_state");
      next.phase = "lobby"; clearRound(next); break;
    case "transfer_leader": {
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      const target = member(next, command.targetEndpointId);
      if (!target?.connected) return reject("not_found");
      if (target.endpointId !== next.leaderEndpointId) {
        next.leaderEndpointId = target.endpointId; next.leaderGeneration++;
        leaderChanged = true;
      }
      break;
    }
    case "close":
      if (next.leaderEndpointId !== command.actorEndpointId) return reject("unauthorized");
      next.phase = "closed"; break;
  }
  if (actor) {
    actor.lastCommandId = command.commandId;
    actor.lastCommandFingerprint = fingerprint;
  }
  retainReceipt(next, command, fingerprint);
  if (next.revision === U32_MAX) return reject("invalid_state");
  next.revision++;
  if (!validMatchLobby(next)) return reject("invalid_state");
  Object.assign(lobby, next);
  return step(lobby, true, "ok", false, leaderChanged);
}
