// Pure MatchRoom snapshot boundary for the browser launcher.
// No DOM, network, storage, Wasm or credential behavior belongs here.
"use strict";

((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.MDKROnlineRoomLiveState = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  const U32_MAX = 0xffff_ffff;
  const U64_MAX = 0xffff_ffff_ffff_ffffn;
  const MAX_ENDPOINTS = 4;
  const MAX_SEATS = 4;
  const MAX_CONTROL_TAIL = 64;
  const LOBBY_PHASE = Object.freeze({lobby: 1, loading: 2, racing: 3,
    results: 4, closed: 5});
  const COMPATIBILITY_KEYS = Object.freeze(["protocolVersion", "buildId",
    "gameplayDigest", "romRevision", "cadenceHz"]);
  const PUBLIC_STATE_KEYS = Object.freeze(["type", "schemaVersion", "expiresAt",
    "inviteExpiresAt", "inviteGeneration", "closedReason", "lobby", "controlTail"]);
  const IDENTITY_KEYS = Object.freeze(["roomId", "endpointId", "credential"]);
  const INVITE_KEYS = Object.freeze(["fallbackCode", "inviteExpiresInMs", "inviteUrl"]);
  const HELD_INVITE_KEYS = Object.freeze(["expiresAt", "fallbackCode",
    "inviteGeneration", "inviteUrl"]);
  const CANONICAL_STATE_KEYS = Object.freeze([...PUBLIC_STATE_KEYS, ...IDENTITY_KEYS]);
  const LOBBY_KEYS = Object.freeze(["protocolVersion", "revision", "matchEpoch",
    "leaderGeneration", "roomId", "leaderEndpointId", "phase", "compatibility",
    "members", "seats", "selectedTrack", "selectedVehicleMask"]);
  const MEMBER_KEYS = Object.freeze(["endpointId", "seatCount", "connected",
    "ready", "loaded"]);
  const SEAT_KEYS = Object.freeze(["endpointId", "selectionRevision", "voteTrack",
    "localIndex", "characterId", "vehicleId"]);
  const CONTROL_KEYS = Object.freeze(["accepted", "duplicate", "leaderChanged",
    "error", "revision", "matchEpoch", "leaderEndpointId", "selectedTrack",
    "selectedVehicleMask"]);

  function bytes(value, count) {
    return Array.isArray(value) && value.length === count &&
      value.every((item) => Number.isInteger(item) && item >= 0 && item <= 255);
  }

  function decimal(value, allowZero = false) {
    if (typeof value !== "string" ||
        !(allowZero ? /^(0|[1-9][0-9]{0,19})$/ : /^[1-9][0-9]{0,19}$/).test(value)) {
      return false;
    }
    try {
      const parsed = BigInt(value);
      return parsed <= U64_MAX && (allowZero || parsed !== 0n);
    } catch (_) { return false; }
  }

  function validCompatibility(value) {
    return Boolean(exactKeys(value, COMPATIBILITY_KEYS) &&
      value.protocolVersion === 1 &&
      bytes(value.buildId, 16) && bytes(value.gameplayDigest, 32) &&
      (value.romRevision === 1 || value.romRevision === 2) &&
      (value.cadenceHz === 25 || value.cadenceHz === 30));
  }

  function sameCompatibility(value, expected) {
    return validCompatibility(value) && validCompatibility(expected) &&
      value.protocolVersion === expected.protocolVersion &&
      value.romRevision === expected.romRevision &&
      value.cadenceHz === expected.cadenceHz &&
      value.buildId.every((byte, index) => byte === expected.buildId[index]) &&
      value.gameplayDigest.every((byte, index) =>
        byte === expected.gameplayDigest[index]);
  }

  function exactKeys(value, expected) {
    if (!value || typeof value !== "object" || Array.isArray(value)) return false;
    const keys = Object.keys(value).sort();
    const wanted = [...expected].sort();
    return keys.length === wanted.length &&
      keys.every((key, index) => key === wanted[index]);
  }

  function validControlTail(value, lobby) {
    if (!Array.isArray(value) || value.length > MAX_CONTROL_TAIL) return false;
    let priorRevision = 0;
    let priorEpoch = 0;
    for (const item of value) {
      if (!exactKeys(item, CONTROL_KEYS) || item.accepted !== true ||
          item.duplicate !== false || typeof item.leaderChanged !== "boolean" ||
          item.error !== "ok" || !Number.isInteger(item.revision) ||
          item.revision < 1 || item.revision > lobby.revision ||
          !Number.isInteger(item.matchEpoch) || item.matchEpoch < 0 ||
          item.matchEpoch > lobby.matchEpoch || !decimal(item.leaderEndpointId) ||
          (item.selectedTrack !== null && (!Number.isInteger(item.selectedTrack) ||
            item.selectedTrack < 0 || item.selectedTrack > 255)) ||
          !Number.isInteger(item.selectedVehicleMask) ||
          item.selectedVehicleMask < 0 || item.selectedVehicleMask > 7 ||
          (priorRevision !== 0 && item.revision !== priorRevision + 1) ||
          item.matchEpoch < priorEpoch) return false;
      priorRevision = item.revision;
      priorEpoch = item.matchEpoch;
    }
    if (value.length === 0) return true;
    const last = value[value.length - 1];
    return priorRevision === lobby.revision &&
      last.matchEpoch === lobby.matchEpoch &&
      last.leaderEndpointId === lobby.leaderEndpointId &&
      last.selectedTrack === lobby.selectedTrack &&
      last.selectedVehicleMask === lobby.selectedVehicleMask;
  }

  function validate(value, expectedCompatibility, localEndpointId = "") {
    const lobby = value?.lobby;
    const local = String(localEndpointId || value?.endpointId || "");
    if (!exactKeys(value, CANONICAL_STATE_KEYS) ||
        value.type !== "match_state" || value.schemaVersion !== 1 ||
        !exactKeys(lobby, LOBBY_KEYS) ||
        lobby.protocolVersion !== 1 || !Number.isInteger(lobby.revision) ||
        lobby.revision < 1 || lobby.revision > U32_MAX ||
        !Number.isInteger(lobby.matchEpoch) || lobby.matchEpoch < 0 ||
        lobby.matchEpoch > U32_MAX || !Number.isInteger(lobby.leaderGeneration) ||
        lobby.leaderGeneration < 1 || lobby.leaderGeneration > U32_MAX ||
        !Object.hasOwn(LOBBY_PHASE, lobby.phase) || !decimal(lobby.roomId) ||
        !decimal(lobby.leaderEndpointId) ||
        !sameCompatibility(lobby.compatibility, expectedCompatibility) ||
        !Array.isArray(lobby.members) || lobby.members.length < 1 ||
        lobby.members.length > MAX_ENDPOINTS || !Array.isArray(lobby.seats) ||
        lobby.seats.length < 1 || lobby.seats.length > MAX_SEATS ||
        !Number.isInteger(lobby.selectedVehicleMask) ||
        lobby.selectedVehicleMask < 0 || lobby.selectedVehicleMask > 7 ||
        (lobby.selectedTrack !== null && (!Number.isInteger(lobby.selectedTrack) ||
          lobby.selectedTrack < 0 || lobby.selectedTrack > 255)) ||
        !Number.isSafeInteger(value.expiresAt) || value.expiresAt <= 0 ||
        !Number.isSafeInteger(value.inviteExpiresAt) || value.inviteExpiresAt < 0 ||
        value.inviteExpiresAt > value.expiresAt ||
        !Number.isInteger(value.inviteGeneration) || value.inviteGeneration < 1 ||
        value.inviteGeneration > U32_MAX ||
        !/^[A-Za-z0-9_-]{22}$/.test(value.roomId) ||
        !decimal(value.endpointId) ||
        !/^[A-Za-z0-9_-]{43}$/.test(value.credential) ||
        ![null, "host_closed", "room_expired"].includes(value.closedReason) ||
        (lobby.phase === "closed") !== (value.closedReason !== null) ||
        !validControlTail(value.controlTail, lobby)) {
      return false;
    }
    const ids = new Set();
    for (const member of lobby.members) {
      if (!exactKeys(member, MEMBER_KEYS) || !decimal(member.endpointId) ||
          ids.has(member.endpointId) ||
          !Number.isInteger(member.seatCount) || member.seatCount < 1 ||
          member.seatCount > 2 || typeof member.connected !== "boolean" ||
          typeof member.ready !== "boolean" || typeof member.loaded !== "boolean") {
        return false;
      }
      ids.add(member.endpointId);
    }
    const seatKeys = new Set();
    const characterIds = new Set();
    for (const seat of lobby.seats) {
      if (!exactKeys(seat, SEAT_KEYS) || !ids.has(seat.endpointId) ||
          !Number.isInteger(seat.localIndex) ||
          seat.localIndex < 0 || seat.localIndex > 1 ||
          !Number.isInteger(seat.selectionRevision) || seat.selectionRevision < 0 ||
          seat.selectionRevision > U32_MAX ||
          (seat.characterId !== null && (!Number.isInteger(seat.characterId) ||
            seat.characterId < 0 || seat.characterId >= 10)) ||
          (seat.vehicleId !== null && (!Number.isInteger(seat.vehicleId) ||
            seat.vehicleId < 0 || seat.vehicleId >= 3)) ||
          (seat.voteTrack !== null && (!Number.isInteger(seat.voteTrack) ||
            seat.voteTrack < 0 || seat.voteTrack > 255)) ||
          ((seat.characterId !== null || seat.vehicleId !== null) !==
            (seat.selectionRevision !== 0)) ||
          (seat.characterId !== null && characterIds.has(seat.characterId))) return false;
      const key = `${seat.endpointId}:${seat.localIndex}`;
      if (seatKeys.has(key)) return false;
      seatKeys.add(key);
      if (seat.characterId !== null) characterIds.add(seat.characterId);
    }
    if (!lobby.members.every((member) => {
      const seats = lobby.seats.filter((seat) => seat.endpointId === member.endpointId);
      return seats.length === member.seatCount &&
        seats.every((seat, index) => seat.localIndex === index) &&
        (!member.ready || seats.every((seat) =>
          seat.characterId !== null && seat.vehicleId !== null));
    }) || !ids.has(lobby.leaderEndpointId) || !ids.has(local)) return false;
    const active = lobby.phase === "loading" || lobby.phase === "racing" ||
      lobby.phase === "results";
    return (!active || (lobby.selectedTrack !== null &&
      lobby.selectedVehicleMask !== 0 && lobby.matchEpoch !== 0 &&
      lobby.seats.every((seat) => seat.characterId !== null &&
        seat.vehicleId !== null &&
        (lobby.selectedVehicleMask & (1 << seat.vehicleId)) !== 0))) &&
      (lobby.phase !== "lobby" || (lobby.selectedTrack === null &&
        lobby.selectedVehicleMask === 0 &&
        lobby.members.every((member) => !member.loaded)));
  }

  function frozenState(value, identity) {
    const compatibility = Object.freeze({
      protocolVersion: value.lobby.compatibility.protocolVersion,
      buildId: Object.freeze([...value.lobby.compatibility.buildId]),
      gameplayDigest: Object.freeze([...value.lobby.compatibility.gameplayDigest]),
      romRevision: value.lobby.compatibility.romRevision,
      cadenceHz: value.lobby.compatibility.cadenceHz,
    });
    const members = Object.freeze(value.lobby.members.map((member) =>
      Object.freeze({endpointId: member.endpointId, seatCount: member.seatCount,
        connected: member.connected, ready: member.ready, loaded: member.loaded})));
    const seats = Object.freeze(value.lobby.seats.map((seat) => Object.freeze({
      endpointId: seat.endpointId, selectionRevision: seat.selectionRevision,
      voteTrack: seat.voteTrack, localIndex: seat.localIndex,
      characterId: seat.characterId, vehicleId: seat.vehicleId})));
    const controlTail = Object.freeze(value.controlTail.map((item) => Object.freeze({
      accepted: item.accepted, duplicate: item.duplicate,
      leaderChanged: item.leaderChanged, error: item.error,
      revision: item.revision, matchEpoch: item.matchEpoch,
      leaderEndpointId: item.leaderEndpointId,
      selectedTrack: item.selectedTrack,
      selectedVehicleMask: item.selectedVehicleMask})));
    const lobby = Object.freeze({protocolVersion: value.lobby.protocolVersion,
      revision: value.lobby.revision, matchEpoch: value.lobby.matchEpoch,
      leaderGeneration: value.lobby.leaderGeneration,
      roomId: value.lobby.roomId,
      leaderEndpointId: value.lobby.leaderEndpointId, phase: value.lobby.phase,
      compatibility, members, seats, selectedTrack: value.lobby.selectedTrack,
      selectedVehicleMask: value.lobby.selectedVehicleMask});
    return Object.freeze({type: value.type, schemaVersion: value.schemaVersion,
      expiresAt: value.expiresAt, inviteExpiresAt: value.inviteExpiresAt,
      inviteGeneration: value.inviteGeneration, closedReason: value.closedReason,
      lobby, controlTail, roomId: identity.roomId,
      endpointId: identity.endpointId, credential: identity.credential});
  }

  // frozenState() rebuilds every nested object from hand-picked fields, which
  // silently strips unknown keys. The exact-shape checks inside validate()
  // therefore cannot fail for nested input once frozenState() has run, so the
  // raw wire objects must be shape-checked before that point.
  function strictNestedShape(value) {
    const lobby = value?.lobby;
    if (!exactKeys(lobby, LOBBY_KEYS) ||
        !exactKeys(lobby.compatibility, COMPATIBILITY_KEYS) ||
        !Array.isArray(lobby.members) || !Array.isArray(lobby.seats) ||
        !Array.isArray(value.controlTail)) return false;
    return lobby.members.every((member) => exactKeys(member, MEMBER_KEYS)) &&
      lobby.seats.every((seat) => exactKeys(seat, SEAT_KEYS)) &&
      value.controlTail.every((item) => exactKeys(item, CONTROL_KEYS));
  }

  function validWireKeys(value) {
    return exactKeys(value, PUBLIC_STATE_KEYS) ||
      exactKeys(value, [...PUBLIC_STATE_KEYS, ...IDENTITY_KEYS]) ||
      exactKeys(value, [...PUBLIC_STATE_KEYS, ...INVITE_KEYS]) ||
      exactKeys(value, [...PUBLIC_STATE_KEYS, ...IDENTITY_KEYS, ...INVITE_KEYS]);
  }

  function validInvite(value, origin) {
    if (!/^\d{6}$/.test(value.fallbackCode) ||
        !Number.isSafeInteger(value.inviteExpiresInMs) ||
        value.inviteExpiresInMs < 1 || value.inviteExpiresInMs > 600_000 ||
        typeof value.inviteUrl !== "string") return false;
    try {
      const base = new URL(origin);
      const url = new URL(value.inviteUrl, base);
      const params = new URLSearchParams(url.hash.slice(1));
      const capability = params.get("match");
      return (url.protocol === "https:" || url.protocol === "http:") &&
        url.origin === base.origin && !url.username && !url.password &&
        url.pathname === "/room/" && !url.search &&
        /^[A-Za-z0-9_-]{43}$/.test(capability || "") &&
        url.hash === `#match=${capability}` &&
        [...params].length === 1;
    } catch (_) { return false; }
  }

  function parseInvite(input, origin) {
    try {
      const compact = String(input ?? "").trim();
      if (compact.length === 0 || compact.length > 512) return null;
      if (/^[0-9]{3} ?[0-9]{3}$/.test(compact)) {
        return Object.freeze({code: compact.replace(" ", "")});
      }
      if (/^[A-Za-z0-9_-]{43}$/.test(compact)) {
        return Object.freeze({capability: compact});
      }
      const base = new URL(origin);
      const url = new URL(compact, base);
      const params = new URLSearchParams(url.hash.slice(1));
      const capability = params.get("match");
      if ((url.protocol !== "https:" && url.protocol !== "http:") ||
          url.origin !== base.origin || url.username || url.password ||
          url.pathname !== "/room/" || url.search || [...params].length !== 1 ||
          !/^[A-Za-z0-9_-]{43}$/.test(capability || "") ||
          url.hash !== `#match=${capability}`) return null;
      return Object.freeze({capability});
    } catch (_) { return null; }
  }

  function validHeldInvite(value, origin) {
    return Boolean(exactKeys(value, HELD_INVITE_KEYS) &&
      Number.isSafeInteger(value.expiresAt) && value.expiresAt > 0 &&
      Number.isInteger(value.inviteGeneration) &&
      value.inviteGeneration >= 1 && value.inviteGeneration <= U32_MAX &&
      validInvite({...value, inviteExpiresInMs: 1}, origin));
  }

  function publicationRelation(previous, next) {
    if (!previous) return "advance";
    if (!validate(previous, previous.lobby.compatibility,
        previous.endpointId) || previous.roomId !== next.roomId ||
        previous.endpointId !== next.endpointId ||
        previous.credential !== next.credential ||
        previous.expiresAt !== next.expiresAt ||
        previous.lobby.roomId !== next.lobby.roomId) return "invalid";
    const pairs = [
      [next.lobby.revision, previous.lobby.revision],
      [next.lobby.matchEpoch, previous.lobby.matchEpoch],
      [next.lobby.leaderGeneration, previous.lobby.leaderGeneration],
      [next.inviteGeneration, previous.inviteGeneration],
    ];
    const lower = pairs.some(([current, prior]) => current < prior);
    const higher = pairs.some(([current, prior]) => current > prior);
    // An invitation deadline is immutable within its generation. Check this
    // before classifying an otherwise older publication: a delayed response
    // is harmless, but a service that rewrites the current generation's
    // deadline is equivocating and must fail closed.
    const closesRoom = previous.closedReason === null &&
      next.closedReason !== null && next.lobby.phase === "closed" &&
      next.lobby.revision > previous.lobby.revision;
    if (next.inviteGeneration === previous.inviteGeneration &&
        next.inviteExpiresAt !== previous.inviteExpiresAt && !closesRoom) {
      return "invalid";
    }
    if (lower) {
      return !higher && next.inviteExpiresAt <= previous.inviteExpiresAt
        ? "obsolete" : "invalid";
    }
    if (next.inviteGeneration > previous.inviteGeneration &&
        next.inviteExpiresAt < previous.inviteExpiresAt) return "invalid";
    if (next.lobby.revision === previous.lobby.revision &&
        (JSON.stringify(next.lobby) !== JSON.stringify(previous.lobby) ||
         JSON.stringify(next.controlTail) !== JSON.stringify(previous.controlTail) ||
         next.closedReason !== previous.closedReason)) return "invalid";
    return higher ? "advance" : "duplicate";
  }

  function ingest(value, priorState, priorInvite, expectedCompatibility, origin, now,
                  expectedInviteResponseGeneration = 0) {
    if (!validWireKeys(value) || !Number.isSafeInteger(now) || now < 0 ||
        !Number.isInteger(expectedInviteResponseGeneration) ||
        expectedInviteResponseGeneration < 0 ||
        expectedInviteResponseGeneration > U32_MAX) return null;
    try {
      const carriesIdentity = IDENTITY_KEYS.every((key) => Object.hasOwn(value, key));
      const carriesInvite = INVITE_KEYS.every((key) => Object.hasOwn(value, key));
      const identity = carriesIdentity
        ? {roomId: value.roomId, endpointId: value.endpointId,
          credential: value.credential}
        : priorState && {roomId: priorState.roomId,
          endpointId: priorState.endpointId, credential: priorState.credential};
      if (!identity || (priorState && carriesIdentity &&
          (identity.roomId !== priorState.roomId ||
           identity.endpointId !== priorState.endpointId ||
           identity.credential !== priorState.credential))) return null;
      if (!strictNestedShape(value)) return null;
      const state = frozenState(value, identity);
      if (!validate(state, expectedCompatibility, identity.endpointId)) return null;
      const relation = publicationRelation(priorState, state);
      if (relation === "invalid") return null;
      if (carriesInvite && (!validInvite(value, origin) ||
          state.lobby.leaderEndpointId !== state.endpointId ||
          state.lobby.phase !== "lobby")) return null;
      let invite = null;
      if (carriesInvite) {
        const sameHeldInvite = validHeldInvite(priorInvite, origin) &&
            priorInvite.inviteGeneration === state.inviteGeneration &&
            priorInvite.inviteUrl === value.inviteUrl &&
            priorInvite.fallbackCode === value.fallbackCode;
        if (validHeldInvite(priorInvite, origin) &&
            priorInvite.inviteGeneration === state.inviteGeneration &&
            !sameHeldInvite) return null;
        // A publication that ties on every ordering pair (revision, matchEpoch,
        // leaderGeneration, inviteGeneration) is not newer, so it may not
        // change invite custody. Re-delivering the identical held secret is
        // idempotent and stays accepted below; delivering *different* secret
        // bytes under tied counters is a delayed or replayed rotate response
        // and must install nothing. Held custody is returned unchanged.
        // Ruled fail-closed during landing review, 2026-08-13.
        if (relation === "duplicate" && !sameHeldInvite) {
          return Object.freeze({obsolete: true, state: null,
            invite: validHeldInvite(priorInvite, origin) &&
              priorInvite.expiresAt > now ? priorInvite : null});
        }
        const keepExisting = sameHeldInvite && priorInvite.expiresAt > now;
        const obsoleteGeneration = relation === "obsolete" && priorState &&
          state.inviteGeneration < priorState.inviteGeneration;
        if (priorState && !keepExisting && !obsoleteGeneration &&
            expectedInviteResponseGeneration !== state.inviteGeneration) {
          return null;
        }
        // The service timestamp and the display wall clock may disagree. Hold
        // the secret against monotonic receipt-relative time instead, expiring
        // up to 5% (at most 30 s) early to cover response transit and timer lag.
        const safetyMs = Math.min(30_000,
          Math.floor(value.inviteExpiresInMs / 20));
        const localExpiresAt = now + value.inviteExpiresInMs - safetyMs;
        if (!Number.isSafeInteger(localExpiresAt) || localExpiresAt <= now) return null;
        invite = keepExisting
          ? priorInvite : Object.freeze({inviteUrl: value.inviteUrl,
            fallbackCode: value.fallbackCode,
            inviteGeneration: state.inviteGeneration,
            expiresAt: localExpiresAt});
      } else if (validHeldInvite(priorInvite, origin) &&
          priorInvite.expiresAt > now && state.lobby.phase === "lobby" &&
          state.lobby.leaderEndpointId === state.endpointId &&
          priorInvite.inviteGeneration === state.inviteGeneration) {
        invite = priorInvite;
      }
      if (relation === "obsolete") {
        // Public state remains on the newer publication, and an older
        // publication may not install invite bytes either: only a strictly
        // newer publication may change custody. A delayed rotate response can
        // therefore preserve custody the launcher already holds, but it can
        // never introduce a secret — that path is indistinguishable from a
        // replayed rotate response. `invite === priorInvite` is exactly the
        // "held, unchanged" case; anything freshly built from this wire is
        // refused. Secrets for older generations remain destroyed.
        // Ruled fail-closed during landing review, 2026-08-13.
        const currentGenerationInvite = carriesInvite && invite &&
          invite === priorInvite && priorState &&
          state.inviteGeneration === priorState.inviteGeneration &&
          priorState.lobby.phase === "lobby" &&
          priorState.lobby.leaderEndpointId === priorState.endpointId
          ? invite : null;
        return Object.freeze({obsolete: true, state: null,
          invite: currentGenerationInvite});
      }
      return Object.freeze({obsolete: false, state, invite});
    } catch (_) { return null; }
  }

  function projection(value, expectedCompatibility, localEndpointId,
                      roomPhase, journey, inviteState) {
    if (!validate(value, expectedCompatibility, localEndpointId) ||
        !Number.isInteger(roomPhase) || roomPhase < 1 || roomPhase > 8 ||
        !Number.isInteger(journey) || journey < 1 || journey > 3 ||
        !Number.isInteger(inviteState) || inviteState < 0 || inviteState > 2) return null;
    const lobby = value.lobby;
    const members = lobby.members;
    const endpoint = String(localEndpointId);
    const localIndex = members.findIndex((member) => member.endpointId === endpoint);
    const leaderIndex = members.findIndex((member) =>
      member.endpointId === lobby.leaderEndpointId);
    const phaseMatches = lobby.phase === "lobby" ? roomPhase <= 3 :
      lobby.phase === "loading" ? (roomPhase === 4 || roomPhase === 5) :
      lobby.phase === "racing" ? roomPhase === 6 :
      lobby.phase === "results" ? roomPhase === 7 : roomPhase === 8;
    if (!phaseMatches || (inviteState !== 0 &&
        (lobby.phase !== "lobby" || localIndex !== leaderIndex))) return null;
    const failure = value.closedReason === "host_closed" ? 14 :
      value.closedReason === "room_expired" ? 15 : 0;
    let memberSeats = 0;
    let ready = 0;
    let connected = 0;
    let loaded = 0;
    members.forEach((member, index) => {
      memberSeats |= (member.seatCount & 3) << (index * 2);
      if (member.ready) ready |= 1 << index;
      if (member.connected) connected |= 1 << index;
      if (member.loaded) loaded |= 1 << index;
    });
    let owners = 0;
    let character = 0;
    let vehicle = 0;
    let vote = 0;
    lobby.seats.forEach((seat, index) => {
      const owner = members.findIndex((member) => member.endpointId === seat.endpointId);
      owners |= (owner & 3) << (index * 2);
      if (seat.characterId !== null) character |= 1 << index;
      if (seat.vehicleId !== null) vehicle |= 1 << index;
      if (seat.voteTrack !== null) vote |= 1 << index;
    });
    return Object.freeze([
      roomPhase, LOBBY_PHASE[lobby.phase], lobby.revision, lobby.matchEpoch,
      leaderIndex, localIndex, members.length, lobby.seats.length, memberSeats,
      ready, connected, loaded, owners, character, vehicle, vote,
      lobby.selectedTrack ?? 0, lobby.selectedVehicleMask, journey, failure,
      inviteState,
    ]);
  }

  return Object.freeze({version: 1, validCompatibility, sameCompatibility,
    parseInvite, validate, ingest, projection});
});
