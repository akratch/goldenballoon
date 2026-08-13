// Launcher-owned Online Room. Production remains an honest zero-I/O gate until
// admission is approved. Once a reviewed release policy and a locally verified
// ROM/build compatibility manifest are both present, this lazily loads the
// standalone Wasm projection built from the exact native C reducers/view model.
// JavaScript owns DOM presentation and launcher routes, never room transitions.
"use strict";

(() => {
  const entryFragment = (() => {
    const raw = location.hash;
    const attempted = raw.startsWith("#match=");
    if (!attempted) return {attempted: false, capability: "", scrubbed: true};
    const fields = new URLSearchParams(raw.slice(1));
    const candidate = [...fields].length === 1 ? fields.get("match") || "" : "";
    const exact = raw === `#match=${candidate}` &&
      /^[A-Za-z0-9_-]{43}$/.test(candidate);
    // Erase before consulting any launcher globals, release policy, model, ROM
    // state or network adapter. If the History API cannot erase the secret,
    // navigate to a clean URL and abandon this handoff in the current task.
    try { history.replaceState(null, "", location.pathname + location.search); }
    catch (_) {
      location.replace(location.pathname + location.search);
      return {attempted: true, capability: "", scrubbed: false};
    }
    return {attempted: true, capability: exact ? candidate : "", scrubbed: true};
  })();
  if (!entryFragment.scrubbed) return;
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
  const verification = byId("online-room-verification");
  const verificationPhrase = byId("online-room-verification-phrase");
  const local = byId("online-room-local");
  const controllers = byId("online-room-controllers");
  const play = byId("play");
  const drop = byId("drop");
  const phone = byId("add-phone-controllers");
  const presenter = globalThis.MDKROnlineRoomPresenter;
  const liveState = globalThis.MDKROnlineRoomLiveState;
  const testConfig = globalThis.__mdkrOnlineRoomTestConfig &&
    typeof globalThis.__mdkrOnlineRoomTestConfig === "object"
    ? globalThis.__mdkrOnlineRoomTestConfig : null;
  const liveFixtureAllowed = testConfig?.liveFixture === true &&
    ["127.0.0.1", "::1", "localhost"].includes(location.hostname);
  const initialLiveConfig = liveFixtureAllowed &&
    globalThis.__mdkrOnlineRoomLiveConfig &&
    typeof globalThis.__mdkrOnlineRoomLiveConfig === "object" &&
    globalThis.__mdkrOnlineRoomLiveConfig.enabled === true
    ? globalThis.__mdkrOnlineRoomLiveConfig : null;
  const testState = testConfig
    ? {renders: [], activations: [], announcements: [], errors: []} : null;
  if (testState) globalThis.__mdkrOnlineRoomTestState = testState;
  if (!trigger || !dialog || !close || !title || !explanation || !gate ||
      !modelRoot || !verification || !verificationPhrase || !local ||
      !controllers || !play || !drop || !phone || !presenter ||
      presenter.version !== 1 || !liveState || liveState.version !== 1) return;

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
  let liveInviteTimer = 0;
  let liveInviteRotating = false;
  let liveSocket = null;
  let liveSocketGeneration = 0;
  let liveSocketTimer = 0;
  let liveSocketStableTimer = 0;
  let liveSocketAttempt = 0;
  let liveOperation = 0;
  let liveJourney = 1;
  let liveRoomPhase = 1;
  let liveCommandId = 1;
  let liveLastOperation = null;
  let liveSelectionSaving = false;
  let currentPresentation = null;
  let pendingEntryCapability = entryFragment.capability;
  entryFragment.capability = "";
  let pendingEntryExpiresAt = pendingEntryCapability ? Date.now() + 10 * 60_000 : 0;
  let pendingEntryTimer = 0;

  let gateTitle = "Online Racing Is Not Enabled in This Build";
  let gateExplanation = "Private rooms are still being qualified. Opening " +
    "this screen makes no network request and cannot enable online racing.";
  const maxLiveResponseBytes = 64 * 1024;

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
    delete auxiliary.dataset.inviteView;
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
    delete auxiliary.dataset.inviteView;
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
    currentPresentation = null;
    hideAuxiliary();
    syncLocalRecovery();
  }

  function serviceUrl(path) {
    const configured = liveConfig?.origin ||
      document.querySelector('meta[name="party-service-origin"]')?.content;
    return new URL(path, configured || location.origin).toString();
  }

  async function readLiveJson(response) {
    const declared = response.headers?.get?.("content-length");
    if (declared && /^\d+$/.test(declared) &&
        Number(declared) > maxLiveResponseBytes) {
      try { await response.body?.cancel?.(); } catch (_) {}
      throw new Error("match_response_too_large");
    }
    if (!response.body?.getReader) {
      const text = await response.text();
      if (new TextEncoder().encode(text).byteLength > maxLiveResponseBytes) {
        throw new Error("match_response_too_large");
      }
      return JSON.parse(text);
    }
    const reader = response.body.getReader();
    const decoder = new TextDecoder("utf-8", {fatal: true});
    let total = 0;
    let text = "";
    try {
      for (;;) {
        const {done, value} = await reader.read();
        if (done) break;
        if (!(value instanceof Uint8Array) ||
            total + value.byteLength > maxLiveResponseBytes) {
          try { await reader.cancel(); } catch (_) {}
          throw new Error("match_response_too_large");
        }
        total += value.byteLength;
        text += decoder.decode(value, {stream: true});
      }
      text += decoder.decode();
      return JSON.parse(text);
    } catch (error) {
      try { await reader.cancel(); } catch (_) {}
      throw error;
    } finally {
      try { reader.releaseLock(); } catch (_) {}
    }
  }

  async function liveRequest(path, body, credential = "") {
    if (testState) {
      testState.requests ||= [];
      testState.requests.push({path,
        bodyKeys: body && typeof body === "object" ? Object.keys(body).sort() : [],
        credential: Boolean(credential)});
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
      const value = await readLiveJson(response).catch(() => ({}));
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

  function liveInviteUsable(now = Date.now()) {
    return Boolean(liveInvite && liveRoom?.lobby?.phase === "lobby" &&
      liveInvite.inviteGeneration === liveRoom.inviteGeneration &&
      Number.isSafeInteger(liveInvite.expiresAt) && now < liveInvite.expiresAt);
  }

  function clearLiveInviteTimer() {
    clearTimeout(liveInviteTimer);
    liveInviteTimer = 0;
  }

  function liveInviteReplacementAllowed() {
    return Boolean(liveRoom?.lobby?.phase === "lobby" &&
      liveRoom.lobby.leaderEndpointId === liveRoom.endpointId);
  }

  function showLiveInviteRecovery() {
    if (!liveInviteReplacementAllowed()) {
      hideAuxiliary();
      return false;
    }
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    auxiliary.hidden = false;
    auxiliary.dataset.inviteView = "expired";
    const heading = document.createElement("h3");
    const copy = document.createElement("span");
    const replace = document.createElement("button");
    heading.textContent = "Invitation Expired";
    copy.textContent = "The old link and room code are no longer shown. Create a new invitation for friends who have not joined yet.";
    replace.type = "button";
    replace.className = "btn btn-primary";
    replace.textContent = "Create New Invitation";
    replace.disabled = liveInviteRotating;
    replace.addEventListener("click", () => void rotateLiveInvite());
    auxiliary.append(heading, copy, replace);
    replace.focus();
    return true;
  }

  function scheduleLiveInviteExpiry() {
    clearLiveInviteTimer();
    if (!liveInviteUsable()) return;
    const operation = liveOperation;
    const roomId = liveRoom.roomId;
    const generation = liveInvite.inviteGeneration;
    const deadline = liveInvite.expiresAt;
    const delay = Math.max(0, Math.min(0x7fff_ffff, deadline - Date.now() + 20));
    liveInviteTimer = setTimeout(() => {
      liveInviteTimer = 0;
      if (operation !== liveOperation || liveRoom?.roomId !== roomId ||
          liveRoom?.inviteGeneration !== generation ||
          liveInvite?.inviteGeneration !== generation) return;
      if (Date.now() < deadline) { scheduleLiveInviteExpiry(); return; }
      const inviteWasVisible = byId("online-room-auxiliary").dataset.inviteView ===
        "active";
      // Secret custody ends before any UI work. A presenter/Wasm failure may
      // preserve the public room snapshot, but can never resurrect this link.
      liveInvite = null;
      try {
        if (!projectLiveState()) throw new Error("projection_failed");
        if (inviteWasVisible) showLiveInviteRecovery();
      } catch (error) {
        showLiveFailure(Object.assign(error, {code: "service_unavailable"}));
      }
    }, delay);
  }

  function projectLiveState() {
    if (!liveRoom?.lobby || !api) return false;
    const lobby = liveRoom.lobby;
    const endpoint = String(liveRoom.endpointId);
    const localIndex = lobby.members.findIndex((member) =>
      member.endpointId === endpoint);
    const leaderIndex = lobby.members.findIndex((member) =>
      member.endpointId === lobby.leaderEndpointId);
    const roomPhase = lobby.phase === "lobby" ? liveRoomPhase :
      ({loading: 4, racing: 6, results: 7, closed: 8}[lobby.phase]);
    const inviteState = localIndex === leaderIndex && lobby.phase === "lobby"
      ? (liveInviteUsable() ? 1 : 2) : 0;
    const fields = liveState.projection(liveRoom, liveConfig?.compatibility,
      endpoint, roomPhase, liveJourney, inviteState);
    if (!fields) return false;
    const projected = api.liveProject(...fields);
    if (!projected) return false;
    return renderShared();
  }

  function mergeLiveState(value, expectedInviteResponseGeneration = 0) {
    const priorRoom = liveRoom;
    const priorInvite = liveInvite;
    const ingested = liveState.ingest(value, priorRoom, priorInvite,
      liveConfig?.compatibility, liveConfig?.origin, Date.now(),
      expectedInviteResponseGeneration);
    if (!ingested) {
      throw Object.assign(new Error("invalid_match_state"),
        {code: "service_unavailable"});
    }
    if (ingested.obsolete) {
      if (!ingested.invite) return true;
      // The public snapshot lost an HTTP/WebSocket race, but this is the only
      // valid copy of the already-current generation's invitation secret.
      // Project it against the newer room without rolling public state back.
      liveInvite = ingested.invite;
      try {
        if (projectLiveState()) {
          scheduleLiveInviteExpiry();
          return true;
        }
      } catch (_) {}
      liveInvite = priorInvite;
      scheduleLiveInviteExpiry();
      throw Object.assign(new Error("projection_failed"),
        {code: "service_unavailable"});
    }
    liveRoom = ingested.state;
    liveInvite = ingested.invite;
    try {
      if (projectLiveState()) {
        if (liveRoom.lobby.phase === "closed") {
          forgetLiveRoom();
          return true;
        }
        scheduleLiveInviteExpiry();
        return true;
      }
    } catch (_) {
      // Roll back below. A malformed Wasm/presenter result is no more entitled
      // to advance credential or invite custody than an explicit projection
      // rejection.
    }
    liveRoom = priorRoom;
    liveInvite = priorInvite && priorRoom &&
      priorInvite.inviteGeneration === priorRoom.inviteGeneration &&
      Date.now() < priorInvite.expiresAt ? priorInvite : null;
    scheduleLiveInviteExpiry();
    throw Object.assign(new Error("projection_failed"),
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
    if (code === "host_closed" || code === "not_found") forgetLiveRoom();
  }

  function closeLiveSocket() {
    liveSocketGeneration++;
    clearTimeout(liveSocketTimer);
    clearTimeout(liveSocketStableTimer);
    liveSocketTimer = 0;
    liveSocketStableTimer = 0;
    const socket = liveSocket;
    liveSocket = null;
    try { socket?.close?.(1000, "launcher_route_changed"); } catch (_) {}
  }

  function markLiveSocketStable(socketGeneration) {
    clearTimeout(liveSocketStableTimer);
    liveSocketStableTimer = setTimeout(() => {
      liveSocketStableTimer = 0;
      if (liveSocket && socketGeneration === liveSocketGeneration) {
        liveSocketAttempt = 0;
      }
    }, 30_000);
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
    if (liveSocketAttempt >= 5) {
      showLiveFailure(Object.assign(new Error("state_reconnect_exhausted"),
        {code: "service_unavailable"}));
      return;
    }
    const delay = Math.min(4000, 250 * (2 ** Math.min(liveSocketAttempt++, 4)));
    liveSocketTimer = setTimeout(() => {
      liveSocketTimer = 0;
      void refreshLive(operation).then((ok) => { if (ok) connectLiveSocket(operation); });
    }, delay);
  }

  function failLiveSocketSetup(operation, socketGeneration) {
    if (operation !== liveOperation ||
        socketGeneration !== liveSocketGeneration) return false;
    // Invalidate any callback the adapter may have retained before throwing,
    // and close a partially constructed native socket without trusting its
    // event-listener installation to have completed.
    closeLiveSocket();
    showLiveFailure(Object.assign(new Error("match_state_transport_failed"),
      {code: "service_unavailable"}));
    scheduleLiveReconnect(operation);
    return false;
  }

  function connectLiveSocket(operation = liveOperation) {
    // A delayed caller from a superseded create/join must not close the newer
    // room's live socket merely by reaching this function.
    if (operation !== liveOperation) return false;
    closeLiveSocket();
    if (!liveRoom?.roomId || !liveRoom.credential) return false;
    const socketGeneration = liveSocketGeneration;
    if (typeof liveConfig?.subscribe === "function") {
      let subscription;
      try {
        subscription = liveConfig.subscribe({...liveRoom}, (value) => {
          if (operation !== liveOperation ||
              socketGeneration !== liveSocketGeneration) return;
          try { mergeLiveState(value); }
          catch (error) { closeLiveSocket(); showLiveFailure(error); }
        }, () => {
          if (operation !== liveOperation ||
              socketGeneration !== liveSocketGeneration) return;
          clearTimeout(liveSocketStableTimer);
          liveSocketStableTimer = 0;
          liveSocketGeneration++;
          liveSocket = null;
          scheduleLiveReconnect(operation);
        });
      } catch (_) {
        return failLiveSocketSetup(operation, socketGeneration);
      }
      if (operation !== liveOperation ||
          socketGeneration !== liveSocketGeneration) {
        try { subscription?.close?.(1000, "superseded_during_subscribe"); }
        catch (_) {}
        return false;
      }
      if (!subscription || typeof subscription.close !== "function") {
        return failLiveSocketSetup(operation, socketGeneration);
      }
      liveSocket = subscription;
      markLiveSocketStable(socketGeneration);
      return true;
    }
    let socket;
    try {
      const url = new URL(`/api/match/${liveRoom.roomId}/connect`, serviceUrl("/"));
      url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(url,
        ["gb-match-v1", `gb-match.${liveRoom.credential}`]);
      liveSocket = socket;
      socket.addEventListener("open", () => {
        if (socket === liveSocket && socketGeneration === liveSocketGeneration) {
          markLiveSocketStable(socketGeneration);
        }
      });
      socket.addEventListener("message", (event) => {
        if (socket !== liveSocket || operation !== liveOperation ||
            socketGeneration !== liveSocketGeneration) return;
        if (typeof event.data !== "string" ||
            event.data.length > maxLiveResponseBytes ||
            new TextEncoder().encode(event.data).byteLength >
              maxLiveResponseBytes) {
          liveSocket = null;
          clearTimeout(liveSocketStableTimer);
          liveSocketStableTimer = 0;
          try { socket.close(1009, "invalid_match_state"); } catch (_) {}
          showLiveFailure(Object.assign(new Error("invalid_match_state"),
            {code: "service_unavailable"}));
          return;
        }
        try { mergeLiveState(JSON.parse(event.data)); }
        catch (error) {
          liveSocket = null;
          clearTimeout(liveSocketStableTimer);
          liveSocketStableTimer = 0;
          try { socket.close(1008, "invalid_match_state"); } catch (_) {}
          showLiveFailure(error);
        }
      });
      socket.addEventListener("close", () => {
        if (socket === liveSocket && socketGeneration === liveSocketGeneration) {
          liveSocket = null;
          clearTimeout(liveSocketStableTimer);
          liveSocketStableTimer = 0;
          scheduleLiveReconnect(operation);
        }
      });
    } catch (_) {
      return failLiveSocketSetup(operation, socketGeneration);
    }
    return true;
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
      liveCommandId = 1;
      mergeLiveState(value);
      liveLastOperation = () => refreshLive();
      return connectLiveSocket(operation);
    } catch (error) {
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    }
  }

  function parseInvite(input) {
    return liveState.parseInvite(input, liveConfig?.origin || location.origin);
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
      liveCommandId = 2;
      mergeLiveState(value);
      liveLastOperation = () => refreshLive();
      return connectLiveSocket(operation);
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
    if (!liveInviteUsable()) {
      liveInvite = null;
      try { projectLiveState(); } catch (_) {}
      return showLiveInviteRecovery();
    }
    const auxiliary = byId("online-room-auxiliary");
    auxiliary.replaceChildren();
    auxiliary.hidden = false;
    auxiliary.dataset.inviteView = "active";
    const heading = document.createElement("h3");
    const copy = document.createElement("span");
    const canvas = document.createElement("canvas");
    const code = document.createElement("output");
    const actions = document.createElement("div");
    const share = document.createElement("button");
    const rotate = document.createElement("button");
    heading.textContent = "Invite a Friend";
    canvas.className = "online-room-qr";
    canvas.setAttribute("aria-label", "Private online room QR code");
    let qrReady = false;
    try { qrReady = renderQr(canvas, liveInvite.inviteUrl); }
    catch (_) { qrReady = false; }
    copy.textContent = qrReady
      ? "Scan this room QR or share the link. Use Add phone controllers for this display's controls."
      : "Share this room link or enter the code. Use Add phone controllers for this display's controls.";
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
      if (!liveInviteUsable()) {
        liveInvite = null;
        try { projectLiveState(); } catch (_) {}
        showLiveInviteRecovery();
        return;
      }
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
      if (!liveInviteUsable()) {
        liveInvite = null;
        try { projectLiveState(); } catch (_) {}
        showLiveInviteRecovery();
        return;
      }
      if (shared) {
        showAuxiliary("Invitation Ready", "The private-room link is ready to send.");
      } else { showAuxiliary("Share the Room Code",
        `Enter ${code.textContent} on your friend's display.`); }
    });
    rotate.addEventListener("click", () => void rotateLiveInvite());
    actions.className = "online-room-inline-actions";
    actions.append(share, rotate);
    auxiliary.append(heading, copy);
    if (qrReady) auxiliary.append(canvas);
    auxiliary.append(code, actions);
    share.focus();
    return true;
  }

  async function rotateLiveInvite() {
    if (!liveRoom?.roomId || !liveRoom.credential || liveInviteRotating ||
        liveRoom.lobby?.phase !== "lobby" ||
        liveRoom.lobby?.leaderEndpointId !== liveRoom.endpointId) return false;
    const operation = liveOperation;
    const expectedInviteGeneration = liveRoom.inviteGeneration;
    liveInviteRotating = true;
    showAuxiliary("Creating New Invitation…",
      "Replacing the old link and room code. Friends already in the room stay connected.");
    try {
      const value = await liveRequest(`/api/match/${liveRoom.roomId}/rotate`,
        {expectedInviteGeneration}, liveRoom.credential);
      if (operation !== liveOperation) return false;
      const responseGeneration = expectedInviteGeneration < 0xffff_ffff
        ? expectedInviteGeneration + 1 : 0;
      mergeLiveState(value, responseGeneration);
      return showLiveInvite();
    } catch (error) {
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    } finally {
      if (operation === liveOperation) liveInviteRotating = false;
    }
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
      // A successful leave revokes this endpoint credential atomically. Close
      // is terminal for the whole room and closes its state sockets. Neither
      // receipt has a useful authenticated state refresh left to perform; the
      // caller clears locally only after this accepted response.
      if (type === "leave" || type === "close") return true;
      return refreshLive(operation);
    } catch (error) {
      if (error?.code === "stale_revision") {
        const refreshed = await refreshLive(operation);
        if (refreshed) showAuxiliary("The Room Changed",
          "Your action was not applied. Review the newest room state and try again.");
        return false;
      }
      if (operation === liveOperation) showLiveFailure(error);
      return false;
    }
  }

  function forgetLiveRoom() {
    liveOperation++;
    closeLiveSocket();
    clearLiveInviteTimer();
    liveRoom = null;
    liveInvite = null;
    liveInviteRotating = false;
    liveLastOperation = null;
    liveSocketAttempt = 0;
    liveRoomPhase = 1;
    liveCommandId = 1;
    announcedKey = "";
  }

  function clearLiveRoom() {
    forgetLiveRoom();
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
    if (!liveState.validCompatibility(liveConfig?.compatibility)) {
      throw new Error("Online Room live adapter requires bounded compatibility data");
    }
    const capability = pendingEntryCapability && Date.now() < pendingEntryExpiresAt
      ? pendingEntryCapability : null;
    pendingEntryCapability = "";
    pendingEntryExpiresAt = 0;
    clearTimeout(pendingEntryTimer);
    pendingEntryTimer = 0;
    if (/^[A-Za-z0-9_-]{43}$/.test(capability || "")) {
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
      status: api.status(), verificationPhrase: api.verificationPhrase(),
      kind: api.kind(), failure: api.failure(),
      announcement: api.announcement(), members: api.members(),
      seats: api.seats(), ready: api.readyCount(),
      admission: Boolean(api.requiresAdmission()),
      localPlay: Boolean(api.localPlayAvailable()),
      timeoutVisible: Boolean(api.timeoutVisible()),
      timeoutTitle: api.timeoutTitle(), timeoutCopy: api.timeoutExplanation(),
      controls: [0, 1, 2, 3].map(readControl),
    };
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
    const admitted = presenter.liveAction(currentPresentation, action,
      {fixture: liveConfig?.fixture === true});
    if (!admitted) {
      showAuxiliary("The Room Changed",
        "That action is no longer available. Review the current room and try again.");
      recordActivation(action, 0);
      return 0;
    }
    if (!admitted.available) {
      const carrier = admitted.route === "carrier_locked";
      showAuxiliary(carrier ? "Secure Setup Is Still Locked" :
        "Online Racing Is Still Locked", carrier
        ? "This action needs the authenticated direct connection and a fresh phrase comparison. Nothing was changed."
        : "Room control is available, but race setup and gameplay stay off until rollback qualification is approved. Nothing was changed.");
      recordActivation(action, 2);
      return 2;
    }
    let result = 0;
    const route = admitted.route;
    if (route === "create") result = await createLiveRoom() ? 1 : 0;
    else if (route === "join") {
      showJoinForm(); result = 2;
    } else if (route === "share_invite") {
      if (liveInviteUsable()) {
        result = showLiveInvite() ? 2 : 0;
      } else {
        liveInvite = null;
        result = await rotateLiveInvite() ? 2 : 0;
      }
    } else if (route === "check_setup") {
      liveRoomPhase = 2;
      if (projectLiveState()) {
        showAuxiliary("Checking Everyone's Setup",
          "Game and ROM compatibility already match. Controller and direct-connection checks remain local to each display.");
        liveRoomPhase = 3;
        result = projectLiveState() ? 1 : 0;
      }
    } else if (route === "connection_details") {
      showAuxiliary("Private Room Connection",
        "Room control is connected. Race transport remains disabled until rollback qualification is approved.");
      result = 2;
    } else if (route === "choose_character") {
      result = await sendLiveCommand("set_character", value,
        localSeatIndex("characterId")) ? 1 : 0;
    } else if (route === "choose_vehicle") {
      result = await sendLiveCommand("set_vehicle", value,
        localSeatIndex("vehicleId")) ? 1 : 0;
    } else if (route === "choose_track") {
      result = await sendLiveCommand("set_vote", value,
        localSeatIndex("voteTrack")) ? 1 : 0;
    } else if (route === "ready") {
      result = await sendLiveCommand("set_ready", 1) ? 1 : 0;
    } else if (route === "change_selection") {
      if (await sendLiveCommand("set_ready", 0)) {
        showLiveSelectionEditor(); result = 2;
      }
    } else if (route === "retry") {
      const recovered = await (liveLastOperation ? liveLastOperation() : refreshLive());
      let connected = true;
      if (recovered && liveRoom?.roomId && !liveSocket) {
        liveSocketAttempt = 0;
        connected = connectLiveSocket(liveOperation);
      }
      result = recovered && connected ? 1 : 0;
    } else if (route === "update") {
      showAuxiliary("Update Safely",
        "Use the newest published release from the same trusted source you installed.");
      result = 2;
    } else if (route === "choose_rom" || route === "play_here") {
      forgetLiveRoom();
      localRoute(action); result = 2;
    } else if (route === "setup_controller") {
      showAuxiliary("Set Up a Controller",
        "Connect a gamepad, use this display, or add a browser-based phone controller.");
      result = 2;
    } else if (route === "connection_doctor") {
      showAuxiliary("Connection Doctor",
        "Check that both displays are online, retry the newest invite, then use local play if the networks cannot connect directly.");
      result = 2;
    } else if (route === "return_home" || route === "leave_room") {
      result = await leaveLiveRoom() ? 1 : 0;
      if (result && route === "return_home") localRoute(action);
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

  function addAction(container, item) {
    if (!item.action || !item.label) return;
    let value = 0;
    if (item.selection) {
      const label = document.createElement("label");
      const name = document.createElement("span");
      const select = document.createElement("select");
      label.className = "online-room-choice";
      name.textContent = item.selection.label;
      for (const option of item.selection.options) {
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
    button.className = item.primary ? "btn btn-primary" : "btn btn-ghost";
    button.textContent = item.label;
    button.disabled = !item.enabled;
    button.dataset.onlineAction = String(item.action);
    if (item.destructive) button.dataset.tone = "destructive";
    button.addEventListener("click", () => activate(item.action, value));
    container.append(button);
  }

  function renderShared() {
    if (!api) return false;
    const model = readModel();
    const playable = !play.disabled && play.dataset.blocked !== "1";
    const presentation = presenter.project(model, {
      live: Boolean(liveConfig), localPlayable: playable,
    });
    currentPresentation = presentation;
    gate.hidden = true;
    modelRoot.hidden = false;
    title.textContent = presentation.title;
    explanation.textContent = presentation.explanation;
    byId("online-room-model-status").textContent = presentation.status;
    byId("online-room-model-counts").textContent = presentation.countsText;
    verification.hidden = !presentation.verification.visible;
    verificationPhrase.textContent = presentation.verification.phrase;
    dialog.dataset.viewKind = String(presentation.kind);
    dialog.dataset.failure = String(presentation.failure);
    dialog.dataset.gallerySlug = presentation.slug;

    const timeout = byId("online-room-timeout");
    timeout.hidden = !presentation.timeout.visible;
    byId("online-room-timeout-title").textContent = presentation.timeout.title;
    byId("online-room-timeout-copy").textContent = presentation.timeout.copy;
    byId("online-room-selection").replaceChildren();
    const actions = byId("online-room-model-actions");
    actions.replaceChildren();
    for (const item of presentation.actions) addAction(actions, item);
    hideAuxiliary();
    leaveConfirmation = false;

    if (presentation.announcement.key !== announcedKey) {
      announcedKey = presentation.announcement.key;
      const announcement = byId("online-room-announcement");
      announcement.setAttribute("aria-live", presentation.announcement.priority);
      announcement.textContent = presentation.announcement.text;
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
      verificationPhrase: string("mdkr_online_browser_verification_phrase"),
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
    if (api.version() !== 4 || api.count() < 30) {
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
        !liveState.validCompatibility(value.compatibility)) return null;
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
      fixture,
    };
    if (fixture) {
      normalized.request = value.request;
      normalized.subscribe = value.subscribe;
    }
    return Object.freeze(normalized);
  }

  function sameLiveConfig(left, right) {
    if (!left || !right || left.origin !== right.origin ||
        left.timeoutMs !== right.timeoutMs || left.fixture !== right.fixture) return false;
    const a = left.compatibility;
    const b = right.compatibility;
    return a.protocolVersion === b.protocolVersion &&
      a.romRevision === b.romRevision && a.cadenceHz === b.cadenceHz &&
      a.buildId.every((byte, index) => byte === b.buildId[index]) &&
      a.gameplayDigest.every((byte, index) => byte === b.gameplayDigest[index]);
  }

  function stopLive() {
    forgetLiveRoom();
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

  if (entryFragment.attempted) {
    const policyEnabled = globalThis.__mdkrOnlineControlReleasePolicy?.enabled === true;
    if (!pendingEntryCapability) {
      gateTitle = "Check the Room Invitation";
      gateExplanation = "This private-room link was not valid and has been " +
        "removed from the address bar. Ask your friend for a new link or use a room code.";
    } else if (!policyEnabled && !initialLiveConfig) {
      pendingEntryCapability = "";
      pendingEntryExpiresAt = 0;
      gateTitle = "Private Rooms Aren’t Enabled in This Build";
      gateExplanation = "This invitation was removed from this browser. Local play " +
        "is still available; use a release where private rooms are enabled to join.";
    } else {
      gateTitle = "Choose a ROM to Join the Private Room";
      gateExplanation = "The invitation is held only in this tab’s memory while you " +
        "choose and verify a supported ROM. It expires automatically within ten minutes.";
      pendingEntryTimer = setTimeout(() => {
        if (!pendingEntryCapability) return;
        pendingEntryCapability = "";
        pendingEntryExpiresAt = 0;
        gateTitle = "Private Room Invitation Expired";
        gateExplanation = "The invitation was removed from this browser. Ask your " +
          "friend for a new link or enter the current six-digit room code.";
        if (!liveConfig) showReleaseGate();
      }, 10 * 60_000);
    }
    showReleaseGate();
  }

  const ready = initialLiveConfig ? configureLive(initialLiveConfig, true) :
    testConfig ? ensureModel() : Promise.resolve(false);

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
    // Gallery mutation is evidence-only. Production callers can inspect the
    // current validated projection, but cannot replace it with a fake case.
    select: (value) => testConfig ? selectCase(value) : false,
    current: () => api ? readModel() : null,
    activate,
    inventory: () => {
      if (!api || !testConfig) return [];
      const prior = selectedIndex;
      const rows = Array.from({length: api.count()}, (_, index) => {
        api.select(index);
        return {index, slug: api.slug()};
      });
      if (prior >= 0) api.select(prior);
      return rows;
    },
    selectedIndex: () => testConfig ? selectedIndex : -1,
  });
  if (entryFragment.attempted) requestAnimationFrame(() => {
    if (!dialog.open) open();
  });
})();
