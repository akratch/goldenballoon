// Launcher-owned Online Room. Production remains an honest zero-I/O gate until
// admission is approved. Once a reviewed release policy and a locally verified
// ROM/build compatibility manifest are both present, this lazily loads the
// standalone Wasm projection built from the exact native C reducers/view model.
// JavaScript owns DOM presentation and launcher routes, never room transitions.
"use strict";

(() => {
  const byId = (id) => document.getElementById(id);
  const ownScript = document.currentScript?.src || "";
  const buildQuery = ownScript ? new URL(ownScript, location.href).search : "";
  const trigger = byId("online-room-open");
  const dialog = byId("online-room-dialog");
  const close = byId("online-room-close");
  const title = byId("online-room-title");
  const explanation = byId("online-room-explanation");
  const gate = byId("online-room-gate");
  const modelRoot = byId("online-room-model");
  const local = byId("online-room-local");
  const controllers = byId("online-room-controllers");
  const play = byId("play");
  const drop = byId("drop");
  const phone = byId("add-phone-controllers");
  const testConfig = globalThis.__mdkrOnlineRoomTestConfig &&
    typeof globalThis.__mdkrOnlineRoomTestConfig === "object"
    ? globalThis.__mdkrOnlineRoomTestConfig : null;
  const initialLiveConfig = globalThis.__mdkrOnlineRoomLiveConfig &&
    typeof globalThis.__mdkrOnlineRoomLiveConfig === "object" &&
    globalThis.__mdkrOnlineRoomLiveConfig.enabled === true
    ? globalThis.__mdkrOnlineRoomLiveConfig : null;
  const testState = testConfig || initialLiveConfig
    ? {renders: [], activations: [], announcements: [], errors: []} : null;
  if (testState) globalThis.__mdkrOnlineRoomTestState = testState;
  if (!trigger || !dialog || !close || !title || !explanation || !gate ||
      !modelRoot || !local || !controllers || !play || !drop || !phone) return;

  const characters = ["Diddy", "Timber", "Pipsy", "Tiptup", "Conker",
    "Bumper", "Banjo", "Krunch", "Drumstick", "T.T."];
  const vehicles = ["Car", "Hovercraft", "Plane"];
  const tracks = [["Ancient Lake", 5], ["Fossil Canyon", 3],
    ["Jungle Falls", 29]];
  let returnFocus = trigger;
  let api = null;
  let moduleReady = null;
  let liveConfig = null;
  let selectedIndex = -1;
  let announcedKey = "";
  let leaveConfirmation = false;
  let completionTimer = 0;
  let liveRoom = null;
  let liveInvite = null;
  let liveSocket = null;
  let liveSocketTimer = 0;
  let liveSocketAttempt = 0;
  let liveOperation = 0;
  let liveJourney = 1;
  let liveRoomPhase = 1;
  let liveCommandId = 1;
  let liveLastOperation = null;
  let liveSelectionSaving = false;

  const gateTitle = "Online Racing Is Not Enabled in This Build";
  const gateExplanation = "Private rooms are still being qualified. Opening " +
    "this screen makes no network request and cannot enable online racing.";

  function syncLocalRecovery() {
    const playable = !play.disabled && play.dataset.blocked !== "1";
    local.textContent = playable ? "Play here" : "Choose ROM";
    byId("online-room-status-title").textContent = playable
      ? "Local play is ready" : "Local play stays available";
    byId("online-room-status-copy").textContent = playable
      ? "Your ROM stays on this display and is ready to play."
      : "Choose a supported ROM to play on this display.";
    controllers.disabled = phone.disabled;
  }

  function showAuxiliary(heading, copy) {
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    const strong = document.createElement("h3");
    const span = document.createElement("span");
    strong.textContent = heading;
    span.textContent = copy;
    auxiliary.append(strong, span);
    auxiliary.hidden = false;
  }

  function hideAuxiliary() {
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.hidden = true;
    auxiliary.replaceChildren();
  }

  function showReleaseGate() {
    gate.hidden = false;
    modelRoot.hidden = true;
    title.textContent = gateTitle;
    explanation.textContent = gateExplanation;
    delete dialog.dataset.viewKind;
    delete dialog.dataset.failure;
    delete dialog.dataset.gallerySlug;
    hideAuxiliary();
    syncLocalRecovery();
  }

  function validCompatibility(value) {
    const bytes = (items, count) => Array.isArray(items) && items.length === count &&
      items.every((item) => Number.isInteger(item) && item >= 0 && item <= 255);
    return value && value.protocolVersion === 1 && bytes(value.buildId, 16) &&
      bytes(value.gameplayDigest, 32) && Number.isInteger(value.romRevision) &&
      value.romRevision > 0 && value.romRevision <= 255 &&
      (value.cadenceHz === 25 || value.cadenceHz === 30);
  }

  function serviceUrl(path) {
    const configured = liveConfig?.origin ||
      document.querySelector('meta[name="party-service-origin"]')?.content;
    return new URL(path, configured || location.origin).toString();
  }

  async function liveRequest(path, body, credential = "") {
    if (testState) {
      testState.requests ||= [];
      testState.requests.push({path, body: structuredClone(body), credential: Boolean(credential)});
    }
    const timeoutMs = Math.max(2000, Math.min(30000,
      Number(liveConfig?.timeoutMs) || 10000));
    if (typeof liveConfig?.request === "function") {
      let timer = 0;
      const timeout = new Promise((_, reject) => {
        timer = setTimeout(() => reject(Object.assign(new Error("service_unavailable"),
          {code: "service_unavailable"})), timeoutMs);
      });
      try {
        const task = liveConfig.request(path,
          {body: structuredClone(body), credential});
        const value = await Promise.race([task, timeout]);
        if (value?.error) throw Object.assign(new Error(value.error), {code: value.error});
        return value;
      } finally { clearTimeout(timer); }
    }
    const headers = {"content-type": "application/json"};
    if (credential) headers.authorization = `Bearer ${credential}`;
    const controller = new AbortController();
    const abortTimer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await fetch(serviceUrl(path), {method: "POST", cache: "no-store",
        credentials: "omit", referrerPolicy: "no-referrer", headers,
        signal: controller.signal, body: JSON.stringify(body || {})});
      const value = await response.json().catch(() => ({}));
      if (!response.ok) {
        // The free edge rate rule returns a provider HTML 429 before the Worker,
        // so it cannot carry our JSON enum. Preserve the same calm capacity UX
        // instead of presenting an unexplained generic outage.
        const code = typeof value.error === "string" ? value.error :
          response.status === 429 ? "rate_limited" : "service_unavailable";
        throw Object.assign(new Error(code), {code, status: response.status});
      }
      return value;
    } catch (error) {
      if (error?.name === "AbortError") throw Object.assign(
        new Error("service_unavailable"), {code: "service_unavailable"});
      throw error;
    } finally { clearTimeout(abortTimer); }
  }

  function sameCompatibility(value) {
    const expected = liveConfig?.compatibility;
    return validCompatibility(value) && validCompatibility(expected) &&
      value.protocolVersion === expected.protocolVersion &&
      value.romRevision === expected.romRevision && value.cadenceHz === expected.cadenceHz &&
      value.buildId.every((byte, index) => byte === expected.buildId[index]) &&
      value.gameplayDigest.every((byte, index) => byte === expected.gameplayDigest[index]);
  }

  function validateLiveState(value) {
    const lobby = value?.lobby;
    const decimal = (item) => {
      if (typeof item !== "string" || !/^[1-9][0-9]{0,19}$/.test(item)) return false;
      try { return BigInt(item) <= 0xffff_ffff_ffff_ffffn; } catch (_) { return false; }
    };
    if (value?.type !== "match_state" || value.schemaVersion !== 1 || !lobby ||
        lobby.protocolVersion !== 1 || !Number.isInteger(lobby.revision) ||
        lobby.revision < 1 || lobby.revision > 0xffff_ffff ||
        !Number.isInteger(lobby.matchEpoch) || lobby.matchEpoch < 0 ||
        lobby.matchEpoch > 0xffff_ffff || !Number.isInteger(lobby.leaderGeneration) ||
        lobby.leaderGeneration < 1 || lobby.leaderGeneration > 0xffff_ffff ||
        !["lobby", "loading", "racing", "results", "closed"].includes(lobby.phase) ||
        !decimal(lobby.roomId) || !decimal(lobby.leaderEndpointId) ||
        !sameCompatibility(lobby.compatibility) ||
        !Array.isArray(lobby.members) || lobby.members.length < 1 ||
        lobby.members.length > 4 || !Array.isArray(lobby.seats) ||
        lobby.seats.length < 1 || lobby.seats.length > 4 ||
        !Number.isInteger(lobby.selectedVehicleMask) ||
        lobby.selectedVehicleMask < 0 || lobby.selectedVehicleMask > 7 ||
        (lobby.selectedTrack !== null && (!Number.isInteger(lobby.selectedTrack) ||
          lobby.selectedTrack < 0 || lobby.selectedTrack > 255)) ||
        !Number.isSafeInteger(value.expiresAt) ||
        !Number.isSafeInteger(value.inviteExpiresAt) ||
        !Number.isInteger(value.inviteGeneration) || value.inviteGeneration < 1 ||
        value.inviteGeneration > 0xffff_ffff ||
        ![null, "host_closed", "room_expired"].includes(value.closedReason) ||
        !Array.isArray(value.controlTail) || value.controlTail.length > 64 ||
        Object.hasOwn(lobby, "receipts") || Object.hasOwn(lobby, "nextReceipt")) return false;
    const ids = new Set();
    for (const member of lobby.members) {
      if (!decimal(member?.endpointId) || ids.has(member.endpointId) ||
          Object.hasOwn(member, "lastCommandId") ||
          Object.hasOwn(member, "lastCommandFingerprint") ||
          !Number.isInteger(member.seatCount) || member.seatCount < 1 ||
          member.seatCount > 2 || typeof member.connected !== "boolean" ||
          typeof member.ready !== "boolean" || typeof member.loaded !== "boolean") return false;
      ids.add(member.endpointId);
    }
    const localCounts = new Map();
    const characterIds = new Set();
    for (const seat of lobby.seats) {
      if (!ids.has(seat?.endpointId) || !Number.isInteger(seat.localIndex) ||
          seat.localIndex < 0 || seat.localIndex > 1 ||
          !Number.isInteger(seat.selectionRevision) || seat.selectionRevision < 0 ||
          (seat.characterId !== null && (!Number.isInteger(seat.characterId) ||
            seat.characterId < 0 || seat.characterId >= characters.length)) ||
          (seat.vehicleId !== null && (!Number.isInteger(seat.vehicleId) ||
            seat.vehicleId < 0 || seat.vehicleId >= vehicles.length)) ||
          (seat.voteTrack !== null && (!Number.isInteger(seat.voteTrack) ||
            seat.voteTrack < 0 || seat.voteTrack > 255)) ||
          ((seat.characterId !== null || seat.vehicleId !== null) !==
            (seat.selectionRevision !== 0)) ||
          (seat.characterId !== null && characterIds.has(seat.characterId))) return false;
      const key = `${seat.endpointId}:${seat.localIndex}`;
      if (localCounts.has(key)) return false;
      localCounts.set(key, true);
      if (seat.characterId !== null) characterIds.add(seat.characterId);
    }
    if (!lobby.members.every((member) => {
      const seats = lobby.seats.filter((seat) => seat.endpointId === member.endpointId);
      return seats.length === member.seatCount &&
        seats.every((seat, index) => seat.localIndex === index) &&
        (!member.ready || seats.every((seat) =>
          seat.characterId !== null && seat.vehicleId !== null));
    }) || !ids.has(lobby.leaderEndpointId) ||
        !ids.has(String(liveRoom?.endpointId || value.endpointId || ""))) return false;
    const active = ["loading", "racing", "results"].includes(lobby.phase);
    if (active && (lobby.selectedTrack === null || lobby.selectedVehicleMask === 0 ||
        lobby.matchEpoch === 0 || lobby.seats.some((seat) =>
          seat.characterId === null || seat.vehicleId === null ||
          (lobby.selectedVehicleMask & (1 << seat.vehicleId)) === 0))) return false;
    return lobby.phase !== "lobby" || (lobby.selectedTrack === null &&
      lobby.selectedVehicleMask === 0 && lobby.members.every((member) => !member.loaded));
  }

  function projectLiveState() {
    if (!liveRoom?.lobby || !api) return false;
    const lobby = liveRoom.lobby;
    const members = lobby.members;
    const endpoint = String(liveRoom.endpointId);
    const localIndex = members.findIndex((member) => member.endpointId === endpoint);
    const leaderIndex = members.findIndex((member) =>
      member.endpointId === lobby.leaderEndpointId);
    let memberSeats = 0;
    let ready = 0;
    let connected = 0;
    let loaded = 0;
    for (let index = 0; index < members.length; index++) {
      memberSeats |= (members[index].seatCount & 3) << (index * 2);
      if (members[index].ready) ready |= 1 << index;
      if (members[index].connected) connected |= 1 << index;
      if (members[index].loaded) loaded |= 1 << index;
    }
    let owners = 0;
    let character = 0;
    let vehicle = 0;
    let vote = 0;
    for (let index = 0; index < lobby.seats.length; index++) {
      const owner = members.findIndex((member) =>
        member.endpointId === lobby.seats[index].endpointId);
      owners |= (owner & 3) << (index * 2);
      if (lobby.seats[index].characterId !== null) character |= 1 << index;
      if (lobby.seats[index].vehicleId !== null) vehicle |= 1 << index;
      if (lobby.seats[index].voteTrack !== null) vote |= 1 << index;
    }
    const lobbyPhase = {lobby: 1, loading: 2, racing: 3, results: 4, closed: 5}
      [lobby.phase];
    const roomPhase = lobby.phase === "lobby" ? liveRoomPhase :
      ({loading: 4, racing: 6, results: 7, closed: 8}[lobby.phase]);
    const inviteReady = Boolean(liveInvite && localIndex === leaderIndex &&
      Date.now() < Number(liveRoom.inviteExpiresAt));
    const projected = api.liveProject(roomPhase, lobbyPhase, lobby.revision,
      lobby.matchEpoch, leaderIndex, localIndex, members.length, lobby.seats.length,
      memberSeats, ready, connected, loaded, owners, character, vehicle, vote,
      lobby.selectedTrack ?? 0, lobby.selectedVehicleMask, liveJourney, 0,
      inviteReady ? 1 : 0);
    if (!projected) return false;
    return renderShared();
  }

  function mergeLiveState(value) {
    const candidate = {...(liveRoom || {}), ...value,
      lobby: value?.lobby || liveRoom?.lobby};
    const prior = liveRoom;
    liveRoom = candidate;
    if (!validateLiveState(candidate)) {
      liveRoom = prior;
      throw Object.assign(new Error("invalid_match_state"),
        {code: "service_unavailable"});
    }
    if (!projectLiveState()) throw Object.assign(new Error("projection_failed"),
      {code: "service_unavailable"});
  }

  function failureSlug(code) {
    if (code === "invite_expired") return "failure-invite-expired";
    if (code === "invalid_invite") return "failure-invite-rotated";
    if (code === "capacity") return "failure-room-full";
    if (code === "service_budget_safe" || code === "rate_limited") {
      return "failure-zero-cost-capacity";
    }
    if (code === "incompatible") return "failure-different-build";
    if (code === "not_found") return "failure-room-expired";
    if (code === "host_closed") return "failure-host-closed";
    return "failure-service-unavailable";
  }

  function showLiveFailure(error) {
    const code = typeof error?.code === "string" ? error.code : "service_unavailable";
    if (testState) testState.errors.push(code);
    announcedKey = "";
    selectCase(failureSlug(code));
  }

  function closeLiveSocket() {
    clearTimeout(liveSocketTimer);
    liveSocketTimer = 0;
    const socket = liveSocket;
    liveSocket = null;
    try { socket?.close?.(1000, "launcher_route_changed"); } catch (_) {}
  }

  async function refreshLive(operation = liveOperation) {
    if (!liveRoom?.roomId || !liveRoom.credential) return false;
    try {
      const state = await liveRequest(`/api/match/${liveRoom.roomId}/state`, {},
        liveRoom.credential);
      if (operation !== liveOperation) return false;
      mergeLiveState(state);
      return true;
    } catch (error) {
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    }
  }

  function scheduleLiveReconnect(operation) {
    if (operation !== liveOperation || !liveRoom?.roomId || liveSocketTimer) return;
    const delay = Math.min(4000, 250 * (2 ** Math.min(liveSocketAttempt++, 4)));
    liveSocketTimer = setTimeout(() => {
      liveSocketTimer = 0;
      void refreshLive(operation).then((ok) => { if (ok) connectLiveSocket(operation); });
    }, delay);
  }

  function connectLiveSocket(operation = liveOperation) {
    closeLiveSocket();
    if (!liveRoom?.roomId || !liveRoom.credential || operation !== liveOperation) return;
    if (typeof liveConfig?.subscribe === "function") {
      liveSocket = liveConfig.subscribe({...liveRoom}, (value) => {
        if (operation !== liveOperation) return;
        try { mergeLiveState(value); } catch (error) { showLiveFailure(error); }
      }, () => scheduleLiveReconnect(operation));
      return;
    }
    const url = new URL(`/api/match/${liveRoom.roomId}/connect`, serviceUrl("/"));
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(url, ["gb-match-v1", `gb-match.${liveRoom.credential}`]);
    liveSocket = socket;
    socket.addEventListener("open", () => { liveSocketAttempt = 0; });
    socket.addEventListener("message", (event) => {
      if (socket !== liveSocket || operation !== liveOperation ||
          typeof event.data !== "string" || event.data.length > 64 * 1024) return;
      try { mergeLiveState(JSON.parse(event.data)); }
      catch (error) { showLiveFailure(error); }
    });
    socket.addEventListener("close", () => {
      if (socket === liveSocket) { liveSocket = null; scheduleLiveReconnect(operation); }
    });
  }

  function renderQr(canvas, url) {
    const qr = globalThis.qrcodegen?.QrCode?.encodeText(url,
      globalThis.qrcodegen.QrCode.Ecc.QUARTILE);
    if (!qr) return false;
    const quiet = 4;
    const scale = Math.max(3, Math.floor(240 / (qr.size + quiet * 2)));
    const side = (qr.size + quiet * 2) * scale;
    canvas.width = side;
    canvas.height = side;
    const context = canvas.getContext("2d", {alpha: false});
    context.fillStyle = "#fff";
    context.fillRect(0, 0, side, side);
    context.fillStyle = "#000";
    for (let y = 0; y < qr.size; y++) {
      for (let x = 0; x < qr.size; x++) {
        if (qr.getModule(x, y)) context.fillRect(
          (x + quiet) * scale, (y + quiet) * scale, scale, scale);
      }
    }
    return true;
  }

  async function createLiveRoom() {
    const operation = ++liveOperation;
    liveJourney = 1;
    liveRoomPhase = 1;
    liveLastOperation = createLiveRoom;
    selectCase("connecting-create");
    try {
      const value = await liveRequest("/api/match/create", {
        compatibility: structuredClone(liveConfig.compatibility), seatCount: 1});
      if (operation !== liveOperation) return false;
      liveInvite = {inviteUrl: value.inviteUrl,
        fallbackCode: value.fallbackCode,
        inviteGeneration: value.inviteGeneration || 1};
      liveCommandId = 1;
      liveRoom = {roomId: value.roomId, endpointId: value.endpointId,
        credential: value.credential};
      mergeLiveState(value);
      liveLastOperation = () => refreshLive();
      connectLiveSocket(operation);
      return true;
    } catch (error) {
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    }
  }

  function parseInvite(input) {
    const compact = String(input || "").trim();
    const code = compact.replace(/\s/g, "");
    if (/^\d{6}$/.test(code)) return {code};
    if (/^[A-Za-z0-9_-]{43}$/.test(compact)) return {capability: compact};
    try {
      const parsed = new URL(compact, location.href);
      const capability = new URLSearchParams(parsed.hash.slice(1)).get("match");
      return /^[A-Za-z0-9_-]{43}$/.test(capability || "")
        ? {capability} : null;
    } catch (_) { return null; }
  }

  async function joinLiveRoom(rawInvite) {
    const parsed = parseInvite(rawInvite);
    if (!parsed) {
      showAuxiliary("Check the Invite",
        "Paste the private-room link or enter its six-digit room code.");
      return false;
    }
    const operation = ++liveOperation;
    liveJourney = 2;
    liveRoomPhase = 1;
    liveLastOperation = () => joinLiveRoom(rawInvite);
    selectCase("connecting-join");
    try {
      const path = parsed.code ? "/api/match/code" : "/api/match/join";
      const value = await liveRequest(path, {...parsed,
        compatibility: structuredClone(liveConfig.compatibility), seatCount: 1});
      if (operation !== liveOperation) return false;
      liveInvite = null;
      liveCommandId = 2;
      liveRoom = {roomId: value.roomId, endpointId: value.endpointId,
        credential: value.credential};
      mergeLiveState(value);
      liveLastOperation = () => refreshLive();
      connectLiveSocket(operation);
      return true;
    } catch (error) {
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    }
  }

  function showJoinForm(prefill = "") {
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    auxiliary.hidden = false;
    const heading = document.createElement("h3");
    const label = document.createElement("label");
    const input = document.createElement("input");
    const actions = document.createElement("div");
    const submit = document.createElement("button");
    const cancel = document.createElement("button");
    heading.textContent = "Join a Private Room";
    label.textContent = "Invite Link or 6-Digit Code";
    input.type = "text";
    input.inputMode = "text";
    input.name = "room-invitation";
    input.autocomplete = "off";
    input.spellcheck = false;
    input.autocapitalize = "none";
    input.enterKeyHint = "go";
    input.maxLength = 512;
    input.placeholder = "Paste link or enter 123 456…";
    input.translate = false;
    input.value = prefill;
    input.setAttribute("aria-describedby", "online-room-join-help");
    const help = document.createElement("span");
    help.id = "online-room-join-help";
    help.textContent = "Room invitations are different from phone-controller invitations.";
    submit.type = cancel.type = "button";
    submit.className = "btn btn-primary";
    cancel.className = "btn btn-ghost";
    submit.textContent = "Join Room";
    cancel.textContent = "Cancel";
    submit.addEventListener("click", () => void joinLiveRoom(input.value));
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") { event.preventDefault(); submit.click(); }
    });
    cancel.addEventListener("click", hideAuxiliary);
    label.append(input);
    actions.className = "online-room-inline-actions";
    actions.append(submit, cancel);
    auxiliary.append(heading, label, help, actions);
    input.focus();
  }

  function showLiveInvite() {
    if (!liveInvite) return;
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    auxiliary.hidden = false;
    const heading = document.createElement("h3");
    const copy = document.createElement("span");
    const canvas = document.createElement("canvas");
    const code = document.createElement("output");
    const actions = document.createElement("div");
    const share = document.createElement("button");
    const rotate = document.createElement("button");
    heading.textContent = "Invite a Friend";
    copy.textContent = "Scan this room QR or share the link. Use Add phone controllers for this display's controls.";
    canvas.className = "online-room-qr";
    canvas.setAttribute("aria-label", "Private online room QR code");
    renderQr(canvas, liveInvite.inviteUrl);
    const rawCode = String(liveInvite.fallbackCode || "");
    code.className = "online-room-code";
    code.translate = false;
    code.textContent = rawCode.replace(/^(...)(...)$/, "$1 $2");
    code.setAttribute("aria-label", `Online room code: ${[...rawCode].join(" ")}`);
    share.type = rotate.type = "button";
    share.className = "btn btn-primary";
    rotate.className = "btn btn-ghost";
    share.textContent = "Share Link";
    rotate.textContent = "Replace Invitation";
    share.addEventListener("click", async () => {
      const inviteUrl = liveInvite.inviteUrl;
      let timer = 0;
      const attempted = (async () => {
        if (navigator.share) await navigator.share({title: "Private online room",
          url: inviteUrl});
        else if (navigator.clipboard?.writeText) {
          await navigator.clipboard.writeText(inviteUrl);
        } else return false;
        return true;
      })().catch(() => false);
      const shared = await Promise.race([attempted, new Promise((resolve) => {
        timer = setTimeout(() => resolve(false), 2000);
      })]);
      clearTimeout(timer);
      if (shared) {
        showAuxiliary("Invitation Ready", "The private-room link is ready to send.");
      } else { showAuxiliary("Share the Room Code",
        `Enter ${code.textContent} on your friend's display.`); }
    });
    rotate.addEventListener("click", () => void rotateLiveInvite());
    actions.className = "online-room-inline-actions";
    actions.append(share, rotate);
    auxiliary.append(heading, copy, canvas, code, actions);
    share.focus();
  }

  async function rotateLiveInvite() {
    if (!liveRoom?.roomId || !liveRoom.credential || !liveInvite) return false;
    const operation = liveOperation;
    try {
      const value = await liveRequest(`/api/match/${liveRoom.roomId}/rotate`,
        {expectedInviteGeneration: liveInvite.inviteGeneration}, liveRoom.credential);
      if (operation !== liveOperation) return false;
      liveInvite = {inviteUrl: value.inviteUrl,
        fallbackCode: value.fallbackCode,
        inviteGeneration: value.inviteGeneration};
      mergeLiveState(value);
      showLiveInvite();
      return true;
    } catch (error) { if (operation === liveOperation) showLiveFailure(error); return false; }
  }

  function localSeatIndex(field) {
    const seats = liveRoom?.lobby?.seats || [];
    const endpoint = String(liveRoom?.endpointId || "");
    const index = seats.findIndex((seat) => seat.endpointId === endpoint &&
      seat[field] === null);
    return index >= 0 ? index : seats.findIndex((seat) => seat.endpointId === endpoint);
  }

  async function sendLiveCommand(type, value = 0, targetEndpointId = "0") {
    if (!liveRoom?.lobby || !liveRoom.credential) return false;
    const operation = liveOperation;
    const commandId = String(liveCommandId++);
    const body = {protocolVersion: 1, expectedRevision: liveRoom.lobby.revision,
      commandId, type, value, targetEndpointId: String(targetEndpointId)};
    const path = `/api/match/${liveRoom.roomId}/command`;
    try {
      let result;
      try { result = await liveRequest(path, body, liveRoom.credential); }
      catch (error) {
        if (!error.status) result = await liveRequest(path, body, liveRoom.credential);
        else throw error;
      }
      if (operation !== liveOperation || !result?.accepted) return false;
      return refreshLive(operation);
    } catch (error) {
      if (error?.code === "stale_revision") return refreshLive(operation);
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    }
  }

  function clearLiveRoom() {
    liveOperation++;
    closeLiveSocket();
    liveRoom = null;
    liveInvite = null;
    liveRoomPhase = 1;
    liveCommandId = 1;
    announcedKey = "";
    selectCase("entry");
  }

  async function leaveLiveRoom() {
    if (!liveRoom?.lobby) { clearLiveRoom(); return true; }
    const endpoint = String(liveRoom.endpointId);
    const isLeader = liveRoom.lobby.leaderEndpointId === endpoint;
    const type = liveRoom.lobby.members.length === 1 && isLeader ? "close" : "leave";
    const accepted = await sendLiveCommand(type);
    if (accepted) clearLiveRoom();
    return accepted;
  }

  async function startLive() {
    if (!validCompatibility(liveConfig?.compatibility)) {
      throw new Error("Online Room live adapter requires bounded compatibility data");
    }
    let capability = new URLSearchParams(location.hash.slice(1)).get("match");
    if (!capability) {
      try {
        const staged = JSON.parse(sessionStorage.getItem(
          "mdkr-online-room-entry-v1") || "null");
        sessionStorage.removeItem("mdkr-online-room-entry-v1");
        if (staged && Number.isSafeInteger(staged.createdAt) &&
            Date.now() - staged.createdAt >= 0 &&
            Date.now() - staged.createdAt <= 60_000) capability = staged.capability;
      } catch (_) {
        try { sessionStorage.removeItem("mdkr-online-room-entry-v1"); } catch (_) {}
      }
    }
    if (/^[A-Za-z0-9_-]{43}$/.test(capability || "")) {
      history.replaceState(null, "", location.pathname + location.search);
      open();
      await joinLiveRoom(capability);
    }
    return true;
  }

  function dismiss() {
    if (dialog.open) dialog.close();
  }

  function localRoute(action) {
    dismiss();
    requestAnimationFrame(() => {
      if (action === 17) drop.focus();
      else if (action === 21 && !play.disabled) play.focus();
      else trigger.focus({preventScroll: true});
    });
  }

  function readControl(slot) {
    return {
      slot,
      action: api.controlAction(slot),
      label: api.controlLabel(slot),
      enabled: Boolean(api.controlEnabled(slot)),
    };
  }

  function readModel() {
    return {
      slug: api.slug(), title: api.title(), explanation: api.explanation(),
      status: api.status(), kind: api.kind(), failure: api.failure(),
      announcement: api.announcement(), members: api.members(),
      seats: api.seats(), ready: api.readyCount(),
      admission: Boolean(api.requiresAdmission()),
      localPlay: Boolean(api.localPlayAvailable()),
      timeoutVisible: Boolean(api.timeoutVisible()),
      timeoutTitle: api.timeoutTitle(), timeoutCopy: api.timeoutExplanation(),
      controls: [0, 1, 2, 3].map(readControl),
    };
  }

  function selectionFor(action) {
    if (action === 6) return {label: "Choose Character", options: characters
      .map((label, value) => ({label, value}))};
    if (action === 7) return {label: "Choose Vehicle", options: vehicles
      .map((label, value) => ({label, value}))};
    if (action === 8) return {label: "Choose Track", options: tracks
      .map(([label, value]) => ({label, value}))};
    return null;
  }

  function recordActivation(action, result) {
    if (testState) testState.activations.push({
      action, result, slug: api ? api.slug() : "",
    });
  }

  function completeLater() {
    if (!testConfig?.autoComplete || !api.pending()) return;
    clearTimeout(completionTimer);
    completionTimer = setTimeout(() => {
      if (api.complete()) renderShared();
    }, 40);
  }

  function confirmLeave() {
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    auxiliary.hidden = false;
    const strong = document.createElement("h3");
    const copy = document.createElement("span");
    const keep = document.createElement("button");
    const leave = document.createElement("button");
    strong.textContent = "Leave This Race?";
    copy.textContent = "The race keeps running for your friends. This display " +
      "will stop the game and leave the private room.";
    keep.type = leave.type = "button";
    keep.className = "btn btn-ghost";
    leave.className = "btn btn-ghost";
    keep.textContent = "Keep Racing";
    leave.textContent = "Leave Race and Room";
    leave.dataset.tone = "destructive";
    keep.addEventListener("click", () => {
      leaveConfirmation = false;
      hideAuxiliary();
    });
    leave.addEventListener("click", () => {
      const result = api.dispatch(25, 0);
      recordActivation(25, result);
      leaveConfirmation = false;
      if (result) {
        renderShared();
        localRoute(23);
      }
    });
    auxiliary.append(strong, copy, keep, leave);
    leave.focus();
  }

  function showLiveSelectionEditor() {
    const seatIndex = localSeatIndex("characterId");
    const seat = liveRoom?.lobby?.seats?.[Math.max(0, seatIndex)];
    if (!seat) return;
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    auxiliary.hidden = false;
    const heading = document.createElement("h3");
    const fields = [["Character", characters, seat.characterId ?? 0],
      ["Vehicle", vehicles, seat.vehicleId ?? 0],
      ["Track", tracks.map((item) => item[0]),
        Math.max(0, tracks.findIndex((item) => item[1] === seat.voteTrack))]];
    const selects = [];
    heading.textContent = "Change Your Selection";
    auxiliary.append(heading);
    for (const [name, options, selected] of fields) {
      const label = document.createElement("label");
      const select = document.createElement("select");
      label.textContent = name;
      options.forEach((option, index) => {
        const node = document.createElement("option");
        node.value = String(index);
        node.textContent = option;
        node.selected = index === selected;
        select.append(node);
      });
      label.append(select);
      auxiliary.append(label);
      selects.push(select);
    }
    const save = document.createElement("button");
    save.type = "button";
    save.className = "btn btn-primary";
    save.textContent = "Save Selection";
    save.addEventListener("click", async () => {
      if (liveSelectionSaving) return;
      const index = liveRoom.lobby.seats.indexOf(seat);
      const character = Number(selects[0].value);
      const vehicle = Number(selects[1].value);
      const track = tracks[Number(selects[2].value)][1];
      liveSelectionSaving = true;
      renderShared();
      let saved = false;
      try {
        if (!(await sendLiveCommand("set_character", character, index))) return;
        if (!(await sendLiveCommand("set_vehicle", vehicle, index))) return;
        if (!(await sendLiveCommand("set_vote", track, index))) return;
        saved = true;
      } finally {
        liveSelectionSaving = false;
        if (saved) projectLiveState();
      }
    });
    auxiliary.append(save);
    selects[0].focus();
  }

  async function activateLive(action, value) {
    if (liveSelectionSaving) return 0;
    let result = 0;
    if (action === 1) result = await createLiveRoom() ? 1 : 0;
    else if (action === 2 || action === 15) {
      showJoinForm(); result = 2;
    } else if (action === 3) {
      showLiveInvite(); result = liveInvite ? 2 : 0;
    } else if (action === 4) {
      liveRoomPhase = 2;
      if (projectLiveState()) {
        showAuxiliary("Checking Everyone's Setup",
          "Game and ROM compatibility already match. Controller and direct-connection checks remain local to each display.");
        liveRoomPhase = 3;
        result = projectLiveState() ? 1 : 0;
      }
    } else if (action === 5) {
      showAuxiliary("Private Room Connection",
        "Room control is connected. Race transport remains disabled until rollback qualification is approved.");
      result = 2;
    } else if (action === 6) {
      result = await sendLiveCommand("set_character", value,
        localSeatIndex("characterId")) ? 1 : 0;
    } else if (action === 7) {
      result = await sendLiveCommand("set_vehicle", value,
        localSeatIndex("vehicleId")) ? 1 : 0;
    } else if (action === 8) {
      result = await sendLiveCommand("set_vote", value,
        localSeatIndex("voteTrack")) ? 1 : 0;
    } else if (action === 9) {
      result = await sendLiveCommand("set_ready", 1) ? 1 : 0;
    } else if (action === 10) {
      if (await sendLiveCommand("set_ready", 0)) {
        showLiveSelectionEditor(); result = 2;
      }
    } else if (action === 14) {
      const recovered = await (liveLastOperation ? liveLastOperation() : refreshLive());
      if (recovered && liveRoom?.roomId && !liveSocket) {
        connectLiveSocket(liveOperation);
      }
      result = recovered ? 1 : 0;
    } else if (action === 16) {
      showAuxiliary("Update Safely",
        "Use the newest published release from the same trusted source you installed.");
      result = 2;
    } else if (action === 17 || action === 21) {
      localRoute(action); result = 2;
    } else if (action === 19) {
      showAuxiliary("Set Up a Controller",
        "Connect a gamepad, use this display, or add a browser-based phone controller.");
      result = 2;
    } else if (action === 20) {
      showAuxiliary("Connection Doctor",
        "Check that both displays are online, retry the newest invite, then use local play if the networks cannot connect directly.");
      result = 2;
    } else if (action === 23 || action === 24) {
      result = await leaveLiveRoom() ? 1 : 0;
      if (result && action === 23) localRoute(action);
    }
    recordActivation(action, result);
    return result;
  }

  function activate(action, value = 0) {
    if (!api || !action) return;
    if (liveConfig) {
      void activateLive(action, value);
      return;
    }
    if (action === 25 && !leaveConfirmation) {
      leaveConfirmation = true;
      recordActivation(action, 2);
      confirmLeave();
      return;
    }
    const result = api.dispatch(action, value);
    recordActivation(action, result);
    if (!result) {
      showAuxiliary("Action Could Not Be Completed",
        "The room changed before that action completed. Review the current state and try again.");
      return;
    }
    if (result === 2) {
      if (action === 5) showAuxiliary("Connection Details",
        "Preview adapter · no addresses, invite secrets, names or input samples in diagnostics.");
      else if (action === 16) showAuxiliary("Update Safely",
        "Automatic updates are not enabled. Use the newest published release from the same trusted source you installed.");
      else if (action === 19) showAuxiliary("Set Up a Controller",
        "Connect a gamepad and press a button, use this screen, or add a browser-based phone controller.");
      else if (action === 20) showAuxiliary("Connection Doctor",
        "Keep each display online, retry from the newest invite, then use local play if these networks still cannot connect directly.");
    } else {
      hideAuxiliary();
      renderShared();
      completeLater();
    }
    if (action === 17 || action === 21 || action === 23) localRoute(action);
  }

  function addAction(container, control, primary, selection) {
    if (!control.action || !control.label) return;
    let value = 0;
    if (selection) {
      const label = document.createElement("label");
      const name = document.createElement("span");
      const select = document.createElement("select");
      label.className = "online-room-choice";
      name.textContent = selection.label;
      for (const option of selection.options) {
        const node = document.createElement("option");
        node.value = String(option.value);
        node.textContent = option.label;
        select.append(node);
      }
      value = Number(select.value);
      select.addEventListener("change", () => { value = Number(select.value); });
      label.append(name, select);
      byId("online-room-selection").append(label);
    }
    const button = document.createElement("button");
    button.type = "button";
    button.className = primary ? "btn btn-primary" : "btn btn-ghost";
    button.textContent = control.label;
    button.disabled = !control.enabled;
    button.dataset.onlineAction = String(control.action);
    if (control.action === 24 || control.action === 25) {
      button.dataset.tone = "destructive";
    }
    button.addEventListener("click", () => activate(control.action, value));
    container.append(button);
  }

  function renderShared() {
    if (!api) return false;
    const model = readModel();
    gate.hidden = true;
    modelRoot.hidden = false;
    title.textContent = model.title;
    explanation.textContent = model.explanation;
    byId("online-room-model-status").textContent = model.status || "Private Room Preview";
    byId("online-room-model-counts").textContent =
      `${model.members} members · ${model.seats} racer seats · ${model.ready} ready`;
    dialog.dataset.viewKind = String(model.kind);
    dialog.dataset.failure = String(model.failure);
    dialog.dataset.gallerySlug = model.slug;

    const timeout = byId("online-room-timeout");
    timeout.hidden = !model.timeoutVisible;
    byId("online-room-timeout-title").textContent = model.timeoutTitle;
    byId("online-room-timeout-copy").textContent = model.timeoutCopy;
    byId("online-room-selection").replaceChildren();
    const actions = byId("online-room-model-actions");
    actions.replaceChildren();
    const timeoutControl = model.timeoutVisible ? model.controls[3] : null;
    const seen = new Set();
    if (timeoutControl?.action) {
      addAction(actions, timeoutControl, true, null);
      seen.add(timeoutControl.action);
    }
    for (const control of model.controls.slice(0, 3)) {
      if (!control.action || seen.has(control.action)) continue;
      const displayed = liveConfig && control.action === 21 &&
          (play.disabled || play.dataset.blocked === "1")
        ? {...control, action: 17, label: "Choose ROM"} : control;
      if (seen.has(displayed.action)) continue;
      addAction(actions, displayed, control.slot === 0,
        control.slot === 0 ? selectionFor(displayed.action) : null);
      seen.add(displayed.action);
    }
    // The shared model deliberately carries local availability separately from
    // room actions. Keep that recovery visible in the live browser surface so
    // entering Online Room never turns local play into a back-navigation hunt.
    if (liveConfig && model.localPlay) {
      const playable = !play.disabled && play.dataset.blocked !== "1";
      const action = playable ? 21 : 17;
      if (!seen.has(action)) addAction(actions, {
        action, label: playable ? "Play Here" : "Choose ROM", enabled: true,
      }, false, null);
    }
    hideAuxiliary();
    leaveConfirmation = false;

    const key = `${model.slug}:${model.kind}:${model.failure}`;
    if (key !== announcedKey) {
      announcedKey = key;
      const announcement = byId("online-room-announcement");
      announcement.setAttribute("aria-live",
        model.announcement === 2 ? "assertive" : "polite");
      announcement.textContent = `${model.title}. ${model.explanation}`;
      if (testState) testState.announcements.push({
        text: announcement.textContent,
        priority: announcement.getAttribute("aria-live"),
      });
    }
    if (testState) testState.renders.push(model);
    if (liveSelectionSaving) {
      actions.querySelectorAll("button, select").forEach((control) => {
        control.disabled = true;
      });
      showAuxiliary("Saving Selection…",
        "Keeping your character, vehicle and track choice together.");
    }
    return true;
  }

  function selectCase(value) {
    if (!api) return false;
    let index = Number.isInteger(value) ? value : -1;
    if (typeof value === "string") {
      for (let candidate = 0; candidate < api.count(); candidate++) {
        if (!api.select(candidate)) return false;
        if (api.slug() === value) { index = candidate; break; }
      }
    }
    if (index < 0 || index >= api.count() || !api.select(index)) return false;
    selectedIndex = index;
    announcedKey = "";
    return renderShared();
  }

  function bindModule(module) {
    const number = (name, args = []) => module.cwrap(name, "number", args);
    const string = (name, args = []) => module.cwrap(name, "string", args);
    api = {
      version: number("mdkr_online_browser_version"),
      count: number("mdkr_online_browser_count"),
      select: number("mdkr_online_browser_select", ["number"]),
      slug: string("mdkr_online_browser_slug"),
      title: string("mdkr_online_browser_title"),
      explanation: string("mdkr_online_browser_explanation"),
      status: string("mdkr_online_browser_status"),
      timeoutTitle: string("mdkr_online_browser_timeout_title"),
      timeoutExplanation: string("mdkr_online_browser_timeout_explanation"),
      kind: number("mdkr_online_browser_kind"),
      failure: number("mdkr_online_browser_failure"),
      announcement: number("mdkr_online_browser_announcement"),
      members: number("mdkr_online_browser_member_count"),
      seats: number("mdkr_online_browser_seat_count"),
      readyCount: number("mdkr_online_browser_ready_count"),
      requiresAdmission: number("mdkr_online_browser_requires_admission"),
      localPlayAvailable: number("mdkr_online_browser_local_play_available"),
      timeoutVisible: number("mdkr_online_browser_timeout_visible"),
      controlAction: number("mdkr_online_browser_control_action", ["number"]),
      controlLabel: string("mdkr_online_browser_control_label", ["number"]),
      controlEnabled: number("mdkr_online_browser_control_enabled", ["number"]),
      dispatch: number("mdkr_online_browser_dispatch", ["number", "number"]),
      pending: number("mdkr_online_browser_pending"),
      complete: number("mdkr_online_browser_complete"),
      liveProject: number("mdkr_online_browser_live_project",
        Array(21).fill("number")),
    };
    if (api.version() !== 1 || api.count() < 30) {
      throw new Error("unsupported Online Room model ABI");
    }
    const wanted = typeof testConfig?.case === "string" ? testConfig.case : "entry";
    if (!selectCase(wanted)) throw new Error("could not select Online Room fixture");
    return true;
  }

  function loadFactory() {
    if (globalThis.createMDKROnlineTools) {
      return Promise.resolve(globalThis.createMDKROnlineTools);
    }
    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = "mdkr-online-tools.js" + buildQuery;
      script.onload = () => globalThis.createMDKROnlineTools
        ? resolve(globalThis.createMDKROnlineTools)
        : reject(new Error("Online Room loader did not define its module factory"));
      script.onerror = () => reject(new Error("could not load Online Room model"));
      document.head.append(script);
    });
  }

  function ensureModel() {
    if (moduleReady) return moduleReady;
    moduleReady = loadFactory()
      .then((factory) => factory({
        locateFile: (path) => path + buildQuery,
        printErr: (message) => { if (testState) testState.errors.push(String(message)); },
      }))
      .then(bindModule)
      .catch((error) => {
        moduleReady = null;
        if (testState) testState.errors.push(String(error));
        throw error;
      });
    return moduleReady;
  }

  function normalizedLiveConfig(value, trustedFixture) {
    if (!value || typeof value !== "object" ||
        !validCompatibility(value.compatibility)) return null;
    const policy = globalThis.__mdkrOnlineControlReleasePolicy;
    const fixture = trustedFixture && typeof value.request === "function" &&
      typeof value.subscribe === "function";
    if (!fixture && (!policy || typeof policy !== "object" ||
        policy.enabled !== true)) return null;
    let origin;
    try {
      origin = new URL(fixture ? (value.origin || location.origin) :
        policy.serviceOrigin, location.href);
    } catch (_) { return null; }
    // The published CSP and service contract are same-origin. Refuse a runtime
    // override instead of silently broadening where capability credentials go.
    if (origin.origin !== location.origin) return null;
    const compatibility = value.compatibility;
    const normalized = {
      compatibility: Object.freeze({
        protocolVersion: 1,
        buildId: Object.freeze([...compatibility.buildId]),
        gameplayDigest: Object.freeze([...compatibility.gameplayDigest]),
        romRevision: compatibility.romRevision,
        cadenceHz: compatibility.cadenceHz,
      }),
      origin: origin.origin + "/",
      timeoutMs: Math.max(2000, Math.min(30000,
        Number(fixture ? value.timeoutMs : policy.timeoutMs) || 10000)),
    };
    if (fixture) {
      normalized.request = value.request;
      normalized.subscribe = value.subscribe;
    }
    return Object.freeze(normalized);
  }

  function sameLiveConfig(left, right) {
    if (!left || !right || left.origin !== right.origin ||
        left.timeoutMs !== right.timeoutMs) return false;
    const a = left.compatibility;
    const b = right.compatibility;
    return a.protocolVersion === b.protocolVersion &&
      a.romRevision === b.romRevision && a.cadenceHz === b.cadenceHz &&
      a.buildId.every((byte, index) => byte === b.buildId[index]) &&
      a.gameplayDigest.every((byte, index) => byte === b.gameplayDigest[index]);
  }

  function stopLive() {
    liveOperation++;
    closeLiveSocket();
    liveRoom = null;
    liveInvite = null;
    liveLastOperation = null;
    liveRoomPhase = 1;
    liveCommandId = 1;
    announcedKey = "";
  }

  async function configureLive(value, trustedFixture = false) {
    const next = normalizedLiveConfig(value, trustedFixture);
    if (!next) return false;
    if (sameLiveConfig(liveConfig, next)) return true;
    stopLive();
    liveConfig = next;
    try {
      await ensureModel();
      await startLive();
      return true;
    } catch (error) {
      liveConfig = null;
      stopLive();
      showReleaseGate();
      throw error;
    }
  }

  function disableLive() {
    liveConfig = null;
    stopLive();
    showReleaseGate();
    return true;
  }

  const ready = testConfig ? ensureModel() : initialLiveConfig
    ? configureLive(initialLiveConfig, true) : Promise.resolve(false);

  function open() {
    returnFocus = document.activeElement instanceof HTMLElement
      ? document.activeElement : trigger;
    syncLocalRecovery();
    dialog.showModal();
    requestAnimationFrame(() => title.focus({preventScroll: true}));
  }

  trigger.addEventListener("click", open);
  close.addEventListener("click", dismiss);
  dialog.addEventListener("close", () => {
    if (returnFocus?.isConnected) returnFocus.focus({preventScroll: true});
  });
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) dismiss();
  });
  local.addEventListener("click", () => {
    const playable = !play.disabled && play.dataset.blocked !== "1";
    dismiss();
    requestAnimationFrame(() => (playable ? play : drop).focus());
  });
  controllers.addEventListener("click", () => {
    if (controllers.disabled) return;
    dismiss();
    requestAnimationFrame(() => phone.click());
  });

  globalThis.MDKROnlineRoom = Object.freeze({
    open, close: dismiss, sync: syncLocalRecovery, ready,
    configure: async (value) => {
      try { return await configureLive(value); }
      catch (error) {
        console.error("[online-room] activation failed:", error);
        return false;
      }
    },
    disable: disableLive,
    enabled: () => Boolean(liveConfig),
    compatibility: () => liveConfig ? structuredClone(liveConfig.compatibility) : null,
    select: selectCase,
    current: () => api ? readModel() : null,
    activate,
    inventory: () => {
      if (!api) return [];
      const prior = selectedIndex;
      const rows = Array.from({length: api.count()}, (_, index) => {
        api.select(index);
        return {index, slug: api.slug()};
      });
      if (prior >= 0) api.select(prior);
      return rows;
    },
    selectedIndex: () => selectedIndex,
  });
})();
