"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const testConfig = globalThis.__mdkrPartyHostTestConfig &&
    typeof globalThis.__mdkrPartyHostTestConfig === "object"
    ? globalThis.__mdkrPartyHostTestConfig : null;
  const testState = testConfig
    ? {requests: [], requestDetails: [], rooms: [], announcements: [], lifecycle: [],
      peerCreations: 0, iceRestarts: 0, channelFailures: 0} : null;
  if (testState) globalThis.__mdkrPartyHostTestState = testState;

  const dialog = $("party-dialog");
  const endDialog = $("party-end-dialog");
  const trigger = $("add-phone-controllers");
  const stageTrigger = $("party-stage-button");
  let romReady = false;
  let room = null;
  let socket = null;
  let deadline = 0;
  let timer = null;
  let startingGame = false;
  let operation = 0;
  let socketReconnectTimer = null;
  let socketReconnectAttempt = 0;
  let leavingPage = false;
  let openedFromStage = false;
  let preserveOnClose = false;
  let inviteActive = false;
  let dialogClosing = false;
  let pendingOpenFromStage = null;
  let hostIdentity = null;
  let endReturnFocus = null;
  const pairingPhrases = new Map();
  const phraseTasks = new Map();
  const peers = new Map();
  let peerGeneration = 0;
  const signaledControllers = new Set();
  const remotePads = Array.from({length: 4}, () => ({
    reserved: false, active: false, owner: 0, connectionSequence: 0,
    packets: [], drops: 0,
    haptics: false, rumble: null,
  }));
  const rtcConfig = Object.freeze({iceServers: [{
    urls: "stun:stun.cloudflare.com:3478",
  }]});
  globalThis.__mdkrPartyOverlayOpen = false;

  function announce(message) {
    $("party-status").textContent = message;
    if (testState) testState.announcements.push(message);
  }

  function show(view) {
    $("party-opening").hidden = view !== "opening";
    $("party-error").hidden = view !== "error";
    $("party-room").hidden = view !== "room";
  }

  function playingNow() {
    return $("stage")?.hidden === false;
  }

  function closeDialog() {
    if (!dialog.open) return;
    dialogClosing = true;
    dialog.close();
  }

  function setOverlayOpen(open) {
    /* C samples this latch at its ordinary input/tick boundary. It is local
     * Party chrome only: future online overlays must keep authored time live. */
    globalThis.__mdkrPartyOverlayOpen = Boolean(open && playingNow());
    if (globalThis.__mdkrPartyOverlayOpen) {
      globalThis.__mdkrNeutralizeLocalTouch?.();
    }
  }

  function connectedGamepads() {
    try {
      return navigator.getGamepads
        ? [...navigator.getGamepads()].filter((pad) => pad && pad.connected)
        : [];
    } catch (_) { return []; }
  }

  function localSources() {
    const sources = [null, null, null, null];
    const pads = connectedGamepads();
    for (let index = 0; index < Math.min(4, pads.length); index++) {
      const raw = String(pads[index].id || "Gamepad")
        .replace(/[\u0000-\u001f\u007f]/g, " ").trim();
      sources[index] = raw ? `Gamepad — ${raw.slice(0, 42)}` : "Gamepad";
    }
    if (!sources[0]) {
      sources[0] = globalThis.__mdkrLocalTouchActive === true
        ? "This screen" : "Keyboard";
    }
    return sources;
  }

  function serviceUrl(path) {
    const configured = document.querySelector('meta[name="party-service-origin"]')?.content;
    return new URL(path, configured || location.origin).toString();
  }

  async function request(path, options = {}) {
    if (testState) {
      testState.requests.push(path);
      testState.requestDetails.push({path,
        body: options.body === undefined
          ? null : JSON.parse(JSON.stringify(options.body))});
    }
    if (testConfig?.request) return testConfig.request(path, options);
    const response = await fetch(serviceUrl(path), {
      method: "POST", cache: "no-store", credentials: "omit",
      headers: options.headers || undefined,
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
    });
    const value = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(value.error || "service_unavailable");
    return value;
  }

  function renderQr(url) {
    const qr = globalThis.qrcodegen.QrCode.encodeText(
      url, globalThis.qrcodegen.QrCode.Ecc.QUARTILE);
    const quiet = 4;
    const scale = Math.max(4, Math.floor(300 / (qr.size + quiet * 2)));
    const side = (qr.size + quiet * 2) * scale;
    const canvas = $("party-qr");
    canvas.width = side;
    canvas.height = side;
    const context = canvas.getContext("2d", {alpha: false});
    context.fillStyle = "#fff";
    context.fillRect(0, 0, side, side);
    context.fillStyle = "#000";
    for (let y = 0; y < qr.size; y++) {
      for (let x = 0; x < qr.size; x++) {
        if (qr.getModule(x, y)) {
          context.fillRect((x + quiet) * scale, (y + quiet) * scale, scale, scale);
        }
      }
    }
  }

  function renderInvite(value) {
    room = {...(room || {}), ...value};
    inviteActive = true;
    deadline = Date.now() + Number(value.inviteExpiresInMs || 120000);
    const code = String(value.fallbackCode || "");
    $("party-code").textContent = code.replace(/^(...)(...)$/, "$1 $2");
    $("party-code").setAttribute("aria-label",
      `Controller room code: ${[...code].join(" ")}`);
    renderQr(value.controllerUrl);
    $("party-extend").disabled = false;
    updateCountdown();
  }

  function activeControllers() {
    return (room?.controllers || []).filter((controller) =>
      controller.phase !== "closed");
  }

  function controllerById(controllerId) {
    return activeControllers().find((controller) =>
      controller.controllerId === controllerId);
  }

  function sendSignal(value) {
    if (testConfig) {
      testState.signals ||= [];
      testState.signals.push(JSON.parse(JSON.stringify(value)));
      if (typeof testConfig.sendSignal === "function") testConfig.sendSignal(value);
      return true;
    }
    if (socket?.readyState !== WebSocket.OPEN) return false;
    const encoded = JSON.stringify(value);
    if (encoded.length > 60 * 1024) return false;
    socket.send(encoded);
    return true;
  }

  function retirePeer(controllerId, releaseReservation = false) {
    const peer = peers.get(controllerId);
    if (peer) {
      peer.retired = true;
      if (peer.recoveryTimer !== null) clearTimeout(peer.recoveryTimer);
      peer.recoveryTimer = null;
      peer.pc.close();
    }
    peers.delete(controllerId);
    const controller = controllerById(controllerId) || peer?.controller;
    if (controller?.seat) {
      const pad = remotePads[controller.seat - 1];
      pad.reserved = !releaseReservation;
      pad.active = false;
      pad.packets.length = 0;
      pad.haptics = false;
      pad.rumble = null;
      if (releaseReservation) {
        pad.owner = 0;
        pad.connectionSequence = 0;
      }
    }
  }

  function markPeerConnected(controller, connected) {
    if (!controller?.seat) return;
    const pad = remotePads[controller.seat - 1];
    pad.reserved = true;
    pad.active = connected;
    pad.owner = Math.max(1, ((Number(controller.leaseGeneration) << 3) |
      Number(controller.seat)) >>> 0);
    pad.connectionSequence = Number(controller.connectionSequence) >>> 0;
    if (!connected) pad.packets.length = 0;
    if (room) renderRoomState({...room, transitionId: room.transitionId});
  }

  function rebuildPeer(controllerId, peer, delay = 300) {
    if (peers.get(controllerId) !== peer || peer.retired) return;
    markPeerConnected(peer.controller, false);
    retirePeer(controllerId, false);
    setTimeout(() => void ensurePeer(controllerId), delay);
  }

  function scheduleIceRestart(controllerId, peer, delay) {
    if (peers.get(controllerId) !== peer || peer.retired || peer.recoveryTimer !== null) return;
    peer.recoveryTimer = setTimeout(() => {
      peer.recoveryTimer = null;
      void restartPeerIce(controllerId, peer);
    }, delay);
  }

  async function restartPeerIce(controllerId, peer) {
    if (peers.get(controllerId) !== peer || peer.retired) return false;
    if (peer.state?.readyState !== "open" || peer.control?.readyState !== "open") {
      rebuildPeer(controllerId, peer);
      return false;
    }
    if (peer.pc.signalingState !== "stable") {
      try {
        await peer.pc.setLocalDescription({type: "rollback"});
      } catch (_) {
        scheduleIceRestart(controllerId, peer, 300);
        return false;
      }
    }
    if (peer.restartAttempt >= 3) {
      rebuildPeer(controllerId, peer);
      return false;
    }
    const wasUnhealthy = peer.pc.connectionState !== "connected";
    peer.restartAttempt++;
    try {
      peer.pc.restartIce?.();
      await peer.pc.setLocalDescription(await peer.pc.createOffer({iceRestart: true}));
      if (!sendSignal({type: "webrtc_offer", to: controllerId,
        peerGeneration: peer.generation,
        sdp: peer.pc.localDescription})) throw new Error("signaling_unavailable");
      if (testState) testState.iceRestarts++;
      if (wasUnhealthy) scheduleIceRestart(controllerId, peer,
        Math.min(4000, 800 * (2 ** (peer.restartAttempt - 1))));
      return true;
    } catch (_) {
      if (peer.pc.signalingState === "have-local-offer") {
        await peer.pc.setLocalDescription({type: "rollback"}).catch(() => {});
      }
      scheduleIceRestart(controllerId, peer,
        Math.min(4000, 300 * (2 ** (peer.restartAttempt - 1))));
      return false;
    }
  }

  function directChannelLost(controllerId, peer) {
    if (peers.get(controllerId) !== peer || peer.retired) return;
    if (testState) testState.channelFailures++;
    rebuildPeer(controllerId, peer);
  }

  async function ensurePeer(controllerId) {
    if (peers.has(controllerId) || !signaledControllers.has(controllerId)) return;
    const controller = controllerById(controllerId);
    if (!controller || !["approved", "leased", "connected"].includes(controller.phase)) return;
    const pc = new RTCPeerConnection(rtcConfig);
    const peer = {pc, controller, generation: ++peerGeneration,
      state: null, control: null, authenticated: false,
      retired: false, restartAttempt: 0, recoveryTimer: null};
    peers.set(controllerId, peer);
    if (testState) testState.peerCreations++;
    const activateIfReady = () => {
      if (peers.get(controllerId) === peer && !peer.retired && peer.authenticated &&
          pc.connectionState === "connected" && state.readyState === "open" &&
          control.readyState === "open") markPeerConnected(controller, true);
    };
    pc.addEventListener("icecandidate", (event) => {
      if (event.candidate) sendSignal({type: "webrtc_ice", to: controllerId,
        peerGeneration: peer.generation,
        candidate: event.candidate.toJSON()});
    });
    pc.addEventListener("connectionstatechange", () => {
      if (peers.get(controllerId) !== peer || peer.retired) return;
      if (pc.connectionState === "connected") {
        if (peer.recoveryTimer !== null) clearTimeout(peer.recoveryTimer);
        peer.recoveryTimer = null;
        peer.restartAttempt = 0;
        activateIfReady();
      } else if (pc.connectionState === "disconnected") {
        markPeerConnected(controller, false);
        scheduleIceRestart(controllerId, peer, 750);
      } else if (pc.connectionState === "failed") {
        markPeerConnected(controller, false);
        scheduleIceRestart(controllerId, peer, 0);
      } else if (pc.connectionState === "closed") {
        rebuildPeer(controllerId, peer);
      }
    });
    const state = pc.createDataChannel("mdkr-pad-state-v1",
      {ordered: false, maxRetransmits: 0});
    state.binaryType = "arraybuffer";
    state.addEventListener("open", activateIfReady);
    state.addEventListener("close", () => directChannelLost(controllerId, peer));
    state.addEventListener("error", () => directChannelLost(controllerId, peer));
    state.addEventListener("message", (event) => {
      const bytes = event.data instanceof ArrayBuffer
        ? new Uint8Array(event.data) : null;
      if (!bytes || bytes.byteLength < 24 || bytes.byteLength > 64 || !controller.seat) return;
      const pad = remotePads[controller.seat - 1];
      if (!pad.active) return;
      if (pad.packets.length >= 64) {
        pad.packets.splice(0, pad.packets.length - 1);
        pad.drops++;
      }
      pad.packets.push(bytes.slice());
    });
    const control = pc.createDataChannel("mdkr-pad-control-v1");
    control.addEventListener("open", () => {
      control.send(JSON.stringify({
        type: "host_ready", protocol: 1, seat: controller.seat,
        leaseGeneration: controller.leaseGeneration,
        connectionSequence: controller.connectionSequence,
      }));
      activateIfReady();
    });
    control.addEventListener("close", () => directChannelLost(controllerId, peer));
    control.addEventListener("error", () => directChannelLost(controllerId, peer));
    control.addEventListener("message", (event) => {
      if (typeof event.data !== "string" || event.data.length > 4096) return;
      try {
        const message = JSON.parse(event.data);
        if (message.type === "controller_ready" && message.protocol === 1 &&
            message.controllerId === controllerId &&
            Number(message.connectionSequence) === Number(controller.connectionSequence)) {
          peer.authenticated = true;
          const pad = remotePads[controller.seat - 1];
          pad.haptics = message.capabilities?.vibration === true;
          pad.rumble = pad.haptics ? (strength) => {
            if (control.readyState !== "open") return false;
            control.send(JSON.stringify({type: "rumble", protocol: 1,
              strength: Math.max(0, Math.min(65535, Number(strength) || 0)),
              durationMs: strength > 0 ? 250 : 0}));
            return true;
          } : null;
          activateIfReady();
          control.send(JSON.stringify({type: "controller_ready_ack"}));
        } else if (message.type === "input_test") {
          control.send(JSON.stringify({type: "input_test_ack", nonce: message.nonce}));
        }
      } catch (_) { /* reliable control still rejects malformed JSON */ }
    });
    peer.state = state;
    peer.control = control;
    try {
      await pc.setLocalDescription(await pc.createOffer());
      if (!sendSignal({type: "webrtc_offer", to: controllerId,
        peerGeneration: peer.generation,
        sdp: pc.localDescription})) throw new Error("signaling_unavailable");
    } catch (_) {
      retirePeer(controllerId, false);
      announce("Could not establish a direct phone connection. Try pairing again.");
    }
  }

  async function handleSignal(message) {
    if (!message || typeof message !== "object") return;
    const controllerId = typeof message.controllerId === "string"
      ? message.controllerId : "";
    if (message.type === "controller_hello" && controllerId.length <= 64) {
      signaledControllers.add(controllerId);
      await ensurePeer(controllerId);
      const existing = peers.get(controllerId);
      if (existing && existing.pc.connectionState !== "connected") {
        scheduleIceRestart(controllerId, existing, 0);
      }
      return;
    }
    if (!controllerId || !controllerById(controllerId)) return;
    const peer = peers.get(controllerId);
    if (!peer) return;
    if (!Number.isSafeInteger(message.peerGeneration) ||
        message.peerGeneration !== peer.generation) return;
    try {
      if (message.type === "webrtc_answer" && message.sdp &&
          JSON.stringify(message.sdp).length <= 60 * 1024) {
        await peer.pc.setRemoteDescription(message.sdp);
      } else if (message.type === "webrtc_ice" && message.candidate &&
                 JSON.stringify(message.candidate).length <= 4096) {
        await peer.pc.addIceCandidate(message.candidate);
      }
    } catch (_) { announce("Ignored an invalid phone connection update."); }
  }

  function button(label, className, action, controllerId, disabled = false,
                  value = null) {
    const element = document.createElement("button");
    element.type = "button";
    element.className = className;
    element.textContent = label;
    element.disabled = disabled;
    element.addEventListener("click", () => void control(
      action, controllerId, element,
      typeof value === "function" ? value() : value));
    return element;
  }

  function phoneAtSeat(controllers, seat) {
    return controllers.find((candidate) => candidate.seat === seat &&
      ["approved", "leased", "connected"].includes(candidate.phase));
  }

  function syncRemoteReservations(controllers) {
    for (let seat = 1; seat <= 4; seat++) {
      const controller = phoneAtSeat(controllers, seat);
      const pad = remotePads[seat - 1];
      if (!controller) {
        pad.reserved = false;
        pad.active = false;
        pad.owner = 0;
        pad.connectionSequence = 0;
        pad.packets.length = 0;
        pad.haptics = false;
        pad.rumble = null;
        continue;
      }
      const owner = Math.max(1, ((Number(controller.leaseGeneration) << 3) |
        Number(controller.seat)) >>> 0);
      const connectionSequence = Number(controller.connectionSequence) >>> 0;
      if (pad.owner !== owner || pad.connectionSequence !== connectionSequence) {
        const existing = peers.get(controller.controllerId);
        if (existing && pad.connectionSequence !== 0 &&
            pad.connectionSequence !== connectionSequence) {
          retirePeer(controller.controllerId, false);
        }
        pad.active = false;
        pad.packets.length = 0;
      }
      pad.reserved = true;
      pad.owner = owner;
      pad.connectionSequence = connectionSequence;
    }
  }

  function seatPicker(controllers) {
    const label = document.createElement("label");
    label.append(document.createTextNode("Controller slot"));
    const select = document.createElement("select");
    select.setAttribute("aria-label", "Controller slot for this phone");
    const sources = localSources();
    const candidates = [];
    for (let seat = 1; seat <= 4; seat++) {
      if (phoneAtSeat(controllers, seat)) continue;
      const option = document.createElement("option");
      option.value = String(seat);
      option.textContent = sources[seat - 1]
        ? `Controller ${seat} — replace ${sources[seat - 1]}`
        : `Controller ${seat} — available`;
      option.dataset.free = sources[seat - 1] ? "false" : "true";
      select.append(option);
      candidates.push(option);
    }
    const recommended = candidates.find((option) => option.dataset.free === "true");
    if (recommended) recommended.selected = true;
    label.append(select);
    return {label, select};
  }

  function ensurePhrase(controller) {
    if (!hostIdentity || !controller.controllerPublicKey ||
        pairingPhrases.has(controller.controllerId) ||
        phraseTasks.has(controller.controllerId) || !room?.roomId) return;
    const task = globalThis.MDKRPartySas.phrase(
      hostIdentity.privateKey, controller.controllerPublicKey, {
        roomId: room.roomId, hostPublicKey: hostIdentity.publicKey,
        controllerPublicKey: controller.controllerPublicKey,
      }).then((value) => {
        pairingPhrases.set(controller.controllerId, value);
        phraseTasks.delete(controller.controllerId);
        if (room) renderRoomState({...room, transitionId: room.transitionId});
      }).catch(() => {
        phraseTasks.delete(controller.controllerId);
        announce("A phone sent an invalid pairing key. Decline it and scan again.");
      });
    phraseTasks.set(controller.controllerId, task);
  }

  function renderRoomState(next) {
    if (!room || (Number(next.transitionId) < Number(room.transitionId || 0))) return;
    room = {...room, ...next};
    const controllers = activeControllers();
    syncRemoteReservations(controllers);
    const pending = controllers.filter((controller) => controller.phase === "pending");
    const pendingList = $("party-pending-list");
    pendingList.replaceChildren();
    $("party-pending-empty").hidden = pending.length !== 0;
    for (const controller of pending) {
      ensurePhrase(controller);
      const verifiedPhrase = pairingPhrases.get(controller.controllerId) ||
        controller.phrase || "Verifying…";
      const item = document.createElement("li");
      const copy = document.createElement("span");
      const name = document.createElement("strong");
      name.textContent = controller.name || "Phone controller";
      const phrase = document.createElement("small");
      phrase.textContent = `Pairing phrase: ${verifiedPhrase}`;
      copy.append(name, phrase);
      const picker = seatPicker(controllers);
      item.append(copy,
        picker.label,
        button("Approve", "btn btn-primary", "approve", controller.controllerId,
          verifiedPhrase === "Verifying…" || !picker.select.value,
          () => ({seat: Number(picker.select.value)})),
        button("Decline", "btn btn-ghost", "reject", controller.controllerId));
      pendingList.append(item);
    }

    let ready = 0;
    const sources = localSources();
    for (const tile of $("party-seats").children) {
      const seat = Number(tile.dataset.seat);
      const controller = phoneAtSeat(controllers, seat);
      const source = controller ? null : sources[seat - 1];
      tile.dataset.ready = controller || source ? "true" : "false";
      tile.querySelector("strong").textContent = controller?.name || `Controller ${seat}`;
      tile.querySelector("small").textContent = controller
        ? (remotePads[seat - 1].active ? "Phone connected" : "Phone reconnecting — neutral")
        : (source || "Available");
      if (controller || source) ready++;
    }
    $("party-ready-count").textContent = `${ready} of 4 ready`;
    $("party-start").disabled = !(romReady && ready > 0);
    if ($("party-stage-count")) $("party-stage-count").textContent = String(ready);
    if (testState) testState.rooms.push(JSON.parse(JSON.stringify(next)));
    if (pending.length) announce(`${pending.length} phone${pending.length === 1 ? "" : "s"} waiting for approval.`);
    const liveIds = new Set(controllers.map((controller) => controller.controllerId));
    for (const controllerId of peers.keys()) {
      if (!liveIds.has(controllerId)) retirePeer(controllerId, true);
    }
    for (const controller of controllers) void ensurePeer(controller.controllerId);
  }

  function connectRoom() {
    if (testConfig) {
      if (testConfig.initialRoomState) renderRoomState(testConfig.initialRoomState);
      return;
    }
    if (!room) return;
    const url = new URL(serviceUrl(`/api/party/${room.roomId}/connect`));
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    const connection = new WebSocket(url, ["gb-control-v1", `gb-host.${room.hostCredential}`]);
    socket = connection;
    connection.addEventListener("open", () => {
      socketReconnectAttempt = 0;
      announce("Controller room connected.");
      for (const [controllerId, peer] of peers) {
        if (peer.pc.connectionState !== "connected") {
          scheduleIceRestart(controllerId, peer, 0);
        }
      }
    });
    connection.addEventListener("message", (event) => {
      try {
        const value = JSON.parse(event.data);
        if (value.type === "room_state") renderRoomState(value);
        else void handleSignal(value);
      } catch (_) { announce("Ignored a malformed room update."); }
    });
    connection.addEventListener("close", () => {
      if (socket !== connection) return;
      socket = null;
      if (!room || leavingPage) return;
      announce("Controller room reconnecting… Phone controls remain safe.");
      const delay = Math.min(8000, 300 * (2 ** socketReconnectAttempt++));
      socketReconnectTimer = setTimeout(connectRoom, delay);
    });
  }

  async function openRoom() {
    const generation = ++operation;
    show("opening");
    try {
      hostIdentity = await globalThis.MDKRPartySas.createIdentity();
      const created = await request("/api/party/create", {
        headers: {"content-type": "application/json"},
        body: {hostPublicKey: hostIdentity.publicKey},
      });
      if (generation !== operation || !dialog.open) return;
      renderInvite(created);
      room.controllers = [];
      room.transitionId = 0;
      show("room");
      connectRoom();
      announce("Private phone controller room ready. Scan the QR code.");
    } catch (error) {
      if (generation !== operation) return;
      $("party-error-message").textContent = error?.message === "service_budget_safe"
        ? "Phone pairing is full right now. Keyboard, gamepads and this screen’s touch controls still work offline."
        : "Check this display’s internet connection and try again.";
      show("error");
      $("party-retry").focus();
    }
  }

  async function control(action, controllerId, source, value = null) {
    if (!room) return;
    if (action === "approve" && !pairingPhrases.has(controllerId) &&
        !controllerById(controllerId)?.phrase) {
      announce("Wait for the pairing phrase before approving this phone.");
      return;
    }
    source.disabled = true;
    try {
      await request(`/api/party/${room.roomId}/${action}`, {
        headers: {"content-type": "application/json",
          authorization: `Bearer ${room.hostCredential}`},
        body: {controllerId, ...(value && typeof value === "object" ? value : {})},
      });
      announce(action === "approve" ? "Phone approved." : "Phone declined.");
    } catch (error) {
      source.disabled = false;
      announce(error?.message === "room_full"
        ? "All four phone controller seats are in use. Decline or remove a phone first."
        : "That controller action did not complete. Try again.");
    }
  }

  async function extendInvite() {
    if (!room) return;
    const generation = ++operation;
    const buttonElement = $("party-extend");
    buttonElement.disabled = true;
    try {
      const value = await request(`/api/party/${room.roomId}/rotate`, {
        headers: {"content-type": "application/json",
          authorization: `Bearer ${room.hostCredential}`},
        body: {expectedInviteGeneration: Number(room.inviteGeneration)},
      });
      if (generation !== operation || !dialog.open) return;
      renderInvite(value);
      show("room");
      announce("Invite extended. The previous QR code is no longer valid.");
    } catch (_) {
      if (generation !== operation || !dialog.open) return;
      buttonElement.disabled = false;
      show("error");
      $("party-error-message").textContent =
        "Approved phones stay connected, but a new invite could not be opened. Try again.";
      announce("Could not extend the invite. Try again.");
    }
  }

  function dismissRoom() {
    preserveOnClose = true;
    ++operation; // invalidate a still-pending create/rotate UI completion
    inviteActive = false;
    deadline = 0;
    setOverlayOpen(false);
    const current = room;
    if (current) {
      void request(`/api/party/${current.roomId}/revoke`, {
        headers: {"content-type": "application/json",
          authorization: `Bearer ${current.hostCredential}`}, body: {},
      }).catch(() => announce(
        "The invite could not be revoked immediately; it still expires within two minutes."));
    }
    closeDialog();
  }

  async function endRoom() {
    if (testState) testState.lifecycle.push("endRoom");
    const ending = room;
    room = null;
    inviteActive = false;
    deadline = 0;
    setOverlayOpen(false);
    ++operation;
    if (socket) { socket.close(1000, "host_closed"); socket = null; }
    if (socketReconnectTimer !== null) {
      clearTimeout(socketReconnectTimer); socketReconnectTimer = null;
    }
    for (const controllerId of [...peers.keys()]) retirePeer(controllerId, true);
    signaledControllers.clear();
    pairingPhrases.clear();
    phraseTasks.clear();
    hostIdentity = null;
    if (ending) {
      void request(`/api/party/${ending.roomId}/close`, {
        headers: {"content-type": "application/json",
          authorization: `Bearer ${ending.hostCredential}`}, body: {},
      }).catch(() => {});
    }
    closeDialog();
  }

  function confirmEndRoom() {
    if (!room || endDialog.open) return;
    endReturnFocus = $("party-end");
    endDialog.showModal();
    requestAnimationFrame(() => $("party-end-cancel").focus());
  }

  function updateCountdown() {
    if (!room) return;
    const remaining = Math.max(0, deadline - Date.now());
    if (remaining === 0) inviteActive = false;
    const seconds = Math.ceil(remaining / 1000);
    $("party-expiry").textContent = remaining
      ? `Invite expires in ${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`
      : "Invite expired — extend to add another phone";
    $("party-extend").textContent = remaining ? "Extend 2 minutes" : "Show a new QR code";
  }

  function setRomReady(ready) {
    romReady = Boolean(ready);
    trigger.disabled = !romReady;
    refreshSources();
  }

  function refreshSources() {
    if (room) {
      renderRoomState({transitionId: room.transitionId || 0,
        controllers: room.controllers || []});
      return;
    }
    const ready = localSources().filter(Boolean).length;
    if ($("party-stage-count")) $("party-stage-count").textContent = String(ready);
  }

  function openSheet(fromStage) {
    if (dialogClosing) {
      /* `close` is queued after dialog.open becomes false. Reopening inside
       * that gap would let the stale close event tear down the new room. */
      pendingOpenFromStage = Boolean(fromStage);
      return;
    }
    if (dialog.open) return;
    if (testState) testState.lifecycle.push(`openSheet:${Boolean(fromStage)}`);
    openedFromStage = Boolean(fromStage);
    preserveOnClose = false;
    startingGame = false;
    $("party-start").textContent = openedFromStage ? "Done" : "Start local game";
    dialog.showModal();
    if (testState) testState.lifecycle.push(`showModal:${dialog.open}`);
    setOverlayOpen(true);
    if (room) {
      renderRoomState({...room, transitionId: room.transitionId});
      if (inviteActive && deadline > Date.now()) {
        show("room");
        announce("Phone controller room reopened.");
      } else {
        show("opening");
        void extendInvite();
      }
    } else {
      void openRoom();
    }
  }

  trigger.addEventListener("click", () => openSheet(false));
  stageTrigger?.addEventListener("click", () => openSheet(true));
  $("party-retry").addEventListener("click", () => {
    if (room) void extendInvite();
    else void openRoom();
  });
  $("party-extend").addEventListener("click", () => void extendInvite());
  $("party-close").addEventListener("click", dismissRoom);
  $("party-end").addEventListener("click", confirmEndRoom);
  $("party-end-cancel").addEventListener("click", () => endDialog.close());
  $("party-end-confirm").addEventListener("click", () => {
    endReturnFocus = null;
    endDialog.close();
    void endRoom();
  });
  endDialog.addEventListener("close", () => {
    if (endReturnFocus?.isConnected && dialog.open) {
      endReturnFocus.focus({preventScroll: true});
    }
    endReturnFocus = null;
  });
  $("party-start").addEventListener("click", () => {
    if ($("party-start").disabled) return;
    if (openedFromStage) {
      dismissRoom();
      return;
    }
    startingGame = true;
    setOverlayOpen(false);
    closeDialog();
    $("play").click();
  });
  dialog.addEventListener("cancel", (event) => {
    event.preventDefault();
    dismissRoom();
  });
  dialog.addEventListener("close", () => {
    if (testState) testState.lifecycle.push(
      `close:${startingGame}:${Boolean(room)}:${preserveOnClose}`);
    const returnToStage = openedFromStage || startingGame;
    setOverlayOpen(false);
    if (!startingGame && room && !preserveOnClose) void endRoom();
    if (returnToStage) {
      (openedFromStage ? stageTrigger : $("canvas"))?.focus({preventScroll: true});
    } else {
      trigger.focus({preventScroll: true});
    }
    openedFromStage = false;
    preserveOnClose = false;
    dialogClosing = false;
    if (pendingOpenFromStage !== null) {
      const fromStage = pendingOpenFromStage;
      pendingOpenFromStage = null;
      queueMicrotask(() => openSheet(fromStage));
    }
  });
  addEventListener("gamepadconnected", refreshSources);
  addEventListener("gamepaddisconnected", refreshSources);
  timer = setInterval(updateCountdown, 1000);
  addEventListener("pagehide", () => {
    leavingPage = true;
    if (timer !== null) clearInterval(timer);
    if (socket) socket.close(1000, "pagehide");
  });

  globalThis.MDKRPartyHost = Object.freeze({
    setRomReady,
    open: () => openSheet(playingNow()),
    refreshSources,
    applyRoomState: renderRoomState,
    receiveSignal: (value) => void handleSignal(value),
    state: () => ({romReady, room: room ? JSON.parse(JSON.stringify(room)) : null}),
    remotePads: () => remotePads,
    ...(testConfig ? {
      testRestartIce: () => {
        const entry = peers.entries().next().value;
        return entry ? restartPeerIce(entry[0], entry[1]) : false;
      },
      testCloseControl: () => {
        const entry = peers.values().next().value;
        if (!entry?.control) return false;
        entry.control.close();
        return true;
      },
    } : {}),
  });
})();
