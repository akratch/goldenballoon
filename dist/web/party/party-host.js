"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const testConfig = globalThis.__mdkrPartyHostTestConfig &&
    typeof globalThis.__mdkrPartyHostTestConfig === "object"
    ? globalThis.__mdkrPartyHostTestConfig : null;
  const releaseEnabled =
    globalThis.__mdkrOnlineControlReleasePolicy?.phonePartyEnabled === true;
  const surfaceEnabled = releaseEnabled || testConfig !== null;
  const testState = testConfig
    ? {requests: [], requestDetails: [], rooms: [], announcements: [], lifecycle: [],
      peerCreations: 0, iceRestarts: 0, channelFailures: 0,
      controlPings: 0, controlPongs: 0} : null;
  if (testState) globalThis.__mdkrPartyHostTestState = testState;

  const dialog = $("party-dialog");
  const endDialog = $("party-end-dialog");
  const removeDialog = $("party-remove-dialog");
  const trigger = $("add-phone-controllers");
  const stageTrigger = $("party-stage-button");
  trigger.hidden = !surfaceEnabled;
  if (stageTrigger) stageTrigger.hidden = !surfaceEnabled;
  let romReady = false;
  let room = null;
  let socket = null;
  let deadline = 0;
  let timer = null;
  let startingGame = false;
  let operation = 0;
  let socketReconnectTimer = null;
  let socketReconnectAttempt = 0;
  let socketGeneration = 0;
  let socketStableTimer = null;
  let socketReconnectExhausted = false;
  let publishedRoomTransition = 0;
  let publishedRoomFingerprint = "";
  let leavingPage = false;
  let openedFromStage = false;
  let preserveOnClose = false;
  let inviteActive = false;
  let inviteRevocation = null;
  let dialogClosing = false;
  let pendingOpenFromStage = null;
  let hostIdentity = null;
  let endReturnFocus = null;
  let removeReturnFocus = null;
  let removeControllerId = "";
  const pairingPhrases = new Map();
  const phraseTasks = new Map();
  const peers = new Map();
  const pageBoundRequests = new Set();
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
  const maxPartyResponseBytes = 16 * 1024;
  const partyRequestTimeoutMs = 10_000;
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

  function loopbackHostname(hostname) {
    const value = String(hostname || "").toLowerCase();
    return value === "localhost" || value.endsWith(".localhost") ||
      value === "127.0.0.1" || value === "[::1]" || value === "::1";
  }

  function trustedPartyUrl(url) {
    return url && !url.username && !url.password &&
      (url.protocol === "https:" ||
        (url.protocol === "http:" && loopbackHostname(url.hostname)));
  }

  function serviceUrl(path) {
    const configured = document.querySelector(
      'meta[name="party-service-origin"]')?.content?.trim() || "";
    const base = new URL(configured || location.origin, location.origin);
    if (!trustedPartyUrl(base) || base.origin !== location.origin ||
        (configured && configured !== base.origin)) {
      throw new Error("service_unavailable");
    }
    const target = new URL(path, base.origin);
    if (!trustedPartyUrl(target) || target.origin !== location.origin ||
        !target.pathname.startsWith("/api/") || target.search || target.hash) {
      throw new Error("service_unavailable");
    }
    return target.toString();
  }

  function exactKeys(value, expected) {
    if (!value || typeof value !== "object" || Array.isArray(value)) return false;
    const keys = Object.keys(value).sort();
    const wanted = [...expected].sort();
    return keys.length === wanted.length &&
      keys.every((key, index) => key === wanted[index]);
  }

  async function readPartyJson(response) {
    const declared = response.headers?.get?.("content-length");
    if (declared && /^\d+$/.test(declared) &&
        Number(declared) > maxPartyResponseBytes) {
      try { await response.body?.cancel?.(); } catch (_) {}
      throw new Error("service_unavailable");
    }
    if (!response.body?.getReader) {
      const text = await response.text();
      if (new TextEncoder().encode(text).byteLength > maxPartyResponseBytes) {
        throw new Error("service_unavailable");
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
            total + value.byteLength > maxPartyResponseBytes) {
          try { await reader.cancel(); } catch (_) {}
          throw new Error("service_unavailable");
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

  async function request(path, options = {}) {
    if (testState) {
      testState.requests.push(path);
      testState.requestDetails.push({path,
        body: options.body === undefined
          ? null : JSON.parse(JSON.stringify(options.body))});
    }
    if (testConfig?.request) {
      let timer = 0;
      const timeout = new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error("service_unavailable")),
          partyRequestTimeoutMs);
      });
      try {
        return await Promise.race([
          Promise.resolve(testConfig.request(path, options)), timeout]);
      } finally { clearTimeout(timer); }
    }
    const controller = new AbortController();
    if (options.pageBound) pageBoundRequests.add(controller);
    const timer = setTimeout(() => controller.abort(), partyRequestTimeoutMs);
    try {
      const response = await fetch(serviceUrl(path), {
        method: "POST", cache: "no-store", credentials: "omit",
        referrerPolicy: "no-referrer", signal: controller.signal,
        headers: options.headers || undefined,
        body: options.body === undefined ? undefined : JSON.stringify(options.body),
      });
      const value = await readPartyJson(response).catch(() => ({}));
      if (!response.ok) throw new Error(
        typeof value.error === "string" ? value.error : "service_unavailable");
      return value;
    } catch (error) {
      if (error?.name === "AbortError") throw new Error("service_unavailable");
      throw error;
    } finally {
      clearTimeout(timer);
      pageBoundRequests.delete(controller);
    }
  }

  function abortPageBoundRequests() {
    for (const controller of pageBoundRequests) controller.abort();
    pageBoundRequests.clear();
  }

  function renderQr(url) {
    const qr = globalThis.qrcodegen?.QrCode?.encodeText(
      url, globalThis.qrcodegen.QrCode.Ecc.QUARTILE);
    if (!qr) return false;
    const quiet = 4;
    const scale = Math.max(4, Math.floor(300 / (qr.size + quiet * 2)));
    const side = (qr.size + quiet * 2) * scale;
    const canvas = $("party-qr");
    canvas.width = side;
    canvas.height = side;
    const context = canvas.getContext("2d", {alpha: false});
    if (!context) return false;
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
    return true;
  }

  function normalizedInvite(value, creating, expectedGeneration = null) {
    const createKeys = ["roomId", "hostCredential", "fallbackCode",
      "inviteGeneration", "inviteExpiresInMs", "controllerUrl"];
    const rotateKeys = ["fallbackCode", "inviteGeneration",
      "inviteExpiresInMs", "controllerUrl"];
    if (!exactKeys(value, creating ? createKeys : rotateKeys) ||
        !/^\d{6}$/.test(value.fallbackCode) ||
        !Number.isInteger(value.inviteGeneration) ||
        value.inviteGeneration < 1 || value.inviteGeneration > 0xffff_ffff ||
        !Number.isSafeInteger(value.inviteExpiresInMs) ||
        value.inviteExpiresInMs < 1 || value.inviteExpiresInMs > 120_000 ||
        (creating && (!/^[A-Za-z0-9_-]{22}$/.test(value.roomId) ||
          !/^[A-Za-z0-9_-]{43}$/.test(value.hostCredential) ||
          value.inviteGeneration !== 1)) ||
        (!creating && (!room || !Number.isInteger(expectedGeneration) ||
          value.inviteGeneration !== expectedGeneration + 1 ||
          ![expectedGeneration, value.inviteGeneration]
            .includes(Number(room.inviteGeneration))))) return null;
    try {
      const url = new URL(value.controllerUrl, location.href);
      const capability = url.hash.slice(1);
      if (!trustedPartyUrl(url) || url.origin !== location.origin ||
          url.pathname !== "/controller/" || url.search ||
          !/^[A-Za-z0-9_-]{43}$/.test(capability) ||
          url.hash !== `#${capability}`) return null;
    } catch (_) { return null; }
    return Object.freeze({...value});
  }

  function normalizedRevocation(value, expectedGeneration) {
    if (!exactKeys(value, ["ok", "transitionId", "inviteGeneration"]) ||
        value.ok !== true || !Number.isSafeInteger(value.transitionId) ||
        value.transitionId < 1 || value.transitionId > 4097 ||
        !Number.isInteger(value.inviteGeneration) ||
        value.inviteGeneration !== expectedGeneration) return null;
    return Object.freeze({...value});
  }

  function clearInvitePresentation(expired, announceExpiry = false) {
    inviteActive = false;
    deadline = 0;
    if (room) {
      const {fallbackCode: _code, controllerUrl: _url, ...retained} = room;
      room = retained;
    }
    const canvas = $("party-qr");
    canvas.width = 1;
    canvas.height = 1;
    const wrap = canvas.closest(".party-qr-wrap");
    if (wrap) wrap.hidden = true;
    $("party-code").textContent = "—— —— ——";
    $("party-code").setAttribute("aria-label", expired
      ? "Controller invite expired" : "No controller invite is displayed");
    $("party-scan-step").textContent = expired
      ? "Invite expired — show a new QR code to add another phone"
      : "Invite hidden";
    $("party-install-copy").textContent =
      "Approved phones stay connected. Show a new invite to add another phone.";
    $("party-expiry").textContent = expired
      ? "Invite expired — approved phones stay connected"
      : "Invite is not displayed";
    $("party-extend").textContent = "Show a new QR code";
    $("party-extend").disabled = false;
    if (announceExpiry) announce(
      "Controller invite expired. Approved phones stay connected.");
  }

  function renderInvite(value, expectedGeneration = null) {
    const creating = !room;
    const normalized = normalizedInvite(value, creating, expectedGeneration);
    if (!normalized) throw new Error("service_unavailable");
    room = {...(room || {}), ...normalized};
    inviteActive = true;
    const safetyMs = Math.min(10_000,
      Math.floor(normalized.inviteExpiresInMs / 20));
    deadline = Date.now() + normalized.inviteExpiresInMs - safetyMs;
    const code = normalized.fallbackCode;
    $("party-code").textContent = code.replace(/^(...)(...)$/, "$1 $2");
    $("party-code").setAttribute("aria-label",
      `Controller room code: ${[...code].join(" ")}`);
    let qrReady = false;
    try { qrReady = renderQr(normalized.controllerUrl); }
    catch (_) { qrReady = false; }
    const wrap = $("party-qr").closest(".party-qr-wrap");
    if (wrap) wrap.hidden = !qrReady;
    $("party-scan-step").textContent = qrReady
      ? "1. Scan with your phone’s camera"
      : "1. Open this site’s controller page on your phone";
    $("party-install-copy").textContent = qrReady
      ? "Nothing to install. The controller opens in Safari or Chrome."
      : "Open this site’s /controller/ page in Safari or Chrome, then enter the code below.";
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
    const connection = socket;
    const generation = socketGeneration;
    if (connection?.readyState !== WebSocket.OPEN) return false;
    try {
      const encoded = JSON.stringify(value);
      if (encoded.length > 60 * 1024 ||
          new TextEncoder().encode(encoded).byteLength > 60 * 1024) return false;
      connection.send(encoded);
      return true;
    } catch (_) {
      failRoomSocket(connection, generation, 4011, "controller_socket_send_failed");
      return false;
    }
  }

  function sendPeerOffer(controllerId, peer) {
    if (!peer.pc.localDescription) return false;
    const sent = sendSignal({type: "webrtc_offer", to: controllerId,
      peerGeneration: peer.generation, sdp: peer.pc.localDescription});
    if (sent) peer.offerSentAt = Date.now();
    return sent;
  }

  function retirePeer(controllerId, releaseReservation = false) {
    const peer = peers.get(controllerId);
    if (peer) {
      peer.retired = true;
      if (peer.recoveryTimer !== null) clearTimeout(peer.recoveryTimer);
      if (peer.pingTimer !== null) clearTimeout(peer.pingTimer);
      peer.recoveryTimer = null;
      peer.pingTimer = null;
      try { peer.pc.close(); } catch (_) {}
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
      peer.remoteReady = false;
      if (!sendPeerOffer(controllerId, peer)) return false;
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

  function scheduleControlPing(controllerId, peer, delay = 5000) {
    if (peers.get(controllerId) !== peer || peer.retired || peer.pingTimer !== null) return;
    peer.pingTimer = setTimeout(() => {
      peer.pingTimer = null;
      if (peers.get(controllerId) !== peer || peer.retired) return;
      if (peer.control?.readyState !== "open") {
        directChannelLost(controllerId, peer);
        return;
      }
      const now = Date.now();
      if (peer.pingOutstandingAt !== 0 && now - peer.pingOutstandingAt >= 15000) {
        directChannelLost(controllerId, peer);
        return;
      }
      if (peer.pingOutstandingAt === 0) {
        peer.pingNonce = (peer.pingNonce + 1) >>> 0;
        try {
          peer.control.send(JSON.stringify({type: "ping", protocol: 1,
            nonce: peer.pingNonce}));
          peer.pingOutstandingAt = now;
          if (testState) testState.controlPings++;
        } catch (_) {
          directChannelLost(controllerId, peer);
          return;
        }
      }
      scheduleControlPing(controllerId, peer);
    }, delay);
  }

  async function ensurePeer(controllerId) {
    if (peers.has(controllerId) || !signaledControllers.has(controllerId)) return;
    const controller = controllerById(controllerId);
    if (!controller || !["approved", "leased", "connected"].includes(controller.phase)) return;
    let pc;
    try { pc = new RTCPeerConnection(rtcConfig); }
    catch (_) {
      announce("Could not open a direct phone connection. The phone can try again.");
      return;
    }
    const peer = {pc, controller, generation: ++peerGeneration,
      state: null, control: null, authenticated: false,
      retired: false, remoteReady: false, offerSentAt: 0,
      restartAttempt: 0, recoveryTimer: null, pingTimer: null,
      pingNonce: 0, pingOutstandingAt: 0};
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
          scheduleControlPing(controllerId, peer);
          control.send(JSON.stringify({type: "controller_ready_ack"}));
        } else if (message.type === "input_test") {
          control.send(JSON.stringify({type: "input_test_ack", nonce: message.nonce}));
        } else if (message.type === "pong" && message.protocol === 1 &&
                   Number.isInteger(message.nonce) &&
                   message.nonce === peer.pingNonce) {
          peer.pingOutstandingAt = 0;
          if (testState) testState.controlPongs++;
        }
      } catch (_) { /* reliable control still rejects malformed JSON */ }
    });
    peer.state = state;
    peer.control = control;
    try {
      await pc.setLocalDescription(await pc.createOffer());
      sendPeerOffer(controllerId, peer);
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
      if (existing && !existing.remoteReady &&
          existing.pc.signalingState === "have-local-offer" &&
          existing.pc.localDescription && Date.now() - existing.offerSentAt >= 250) {
        sendPeerOffer(controllerId, existing);
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
        peer.remoteReady = true;
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

  function resetRemotePads() {
    for (const pad of remotePads) {
      pad.reserved = false;
      pad.active = false;
      pad.owner = 0;
      pad.connectionSequence = 0;
      pad.packets.length = 0;
      pad.drops = 0;
      pad.haptics = false;
      pad.rumble = null;
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

  const controllerPhases = new Set([
    "pending", "approved", "leased", "connected",
  ]);

  function normalizedPublishedRoomState(value) {
    const roomKeys = ["type", "transitionId", "inviteExpiresAt",
      "inviteGeneration", "phase", "controllers"];
    if (!exactKeys(value, roomKeys) || value.type !== "room_state" ||
        !Number.isSafeInteger(value.transitionId) || value.transitionId < 1 ||
        value.transitionId > 4097 ||
        !Number.isSafeInteger(value.inviteExpiresAt) ||
        value.inviteExpiresAt < 0 ||
        !Number.isInteger(value.inviteGeneration) ||
        value.inviteGeneration < 1 || value.inviteGeneration > 0xffff_ffff ||
        !["open", "closed"].includes(value.phase) ||
        !Array.isArray(value.controllers) || value.controllers.length > 12) {
      return null;
    }
    const controllerKeys = ["controllerId", "name", "controllerPublicKey",
      "phase", "seat", "leaseGeneration", "connectionSequence"];
    const ids = new Set();
    const seats = new Set();
    const controllers = [];
    for (const controller of value.controllers) {
      if (!exactKeys(controller, controllerKeys) ||
          !/^[A-Za-z0-9_-]{22}$/.test(controller.controllerId) ||
          ids.has(controller.controllerId) ||
          typeof controller.name !== "string" ||
          controller.name.normalize("NFC") !== controller.name ||
          controller.name.trim() !== controller.name ||
          [...controller.name].length > 24 ||
          /[\u0000-\u001f\u007f-\u009f\u200b-\u200f\u2028\u2029\u202a-\u202e\u2060\u2066-\u2069\ufeff]/
            .test(controller.name) ||
          !/^[A-Za-z0-9_-]{87}$/.test(controller.controllerPublicKey) ||
          !controllerPhases.has(controller.phase) ||
          !Number.isInteger(controller.leaseGeneration) ||
          controller.leaseGeneration < 0 ||
          controller.leaseGeneration > 0xffff_ffff ||
          !Number.isInteger(controller.connectionSequence) ||
          controller.connectionSequence < 1 ||
          controller.connectionSequence > 0xffff_ffff) return null;
      if (controller.phase === "pending") {
        if (controller.seat !== null || controller.leaseGeneration !== 0) return null;
      } else if (!Number.isInteger(controller.seat) ||
                 controller.seat < 1 || controller.seat > 4 ||
                 controller.leaseGeneration < 1 || seats.has(controller.seat)) {
        return null;
      }
      ids.add(controller.controllerId);
      if (controller.seat !== null) seats.add(controller.seat);
      controllers.push(Object.freeze({...controller}));
    }
    return Object.freeze({
      type: "room_state", transitionId: value.transitionId,
      inviteExpiresAt: value.inviteExpiresAt,
      inviteGeneration: value.inviteGeneration, phase: value.phase,
      controllers: Object.freeze(controllers),
    });
  }

  function applyPublishedRoomState(value) {
    const normalized = normalizedPublishedRoomState(value);
    if (!normalized) throw new Error("invalid_party_state");
    if (!room || normalized.transitionId < Number(room.transitionId || 0)) return false;
    const fingerprint = JSON.stringify(normalized);
    if (normalized.transitionId === publishedRoomTransition) {
      if (fingerprint !== publishedRoomFingerprint) {
        throw new Error("equivocated_party_state");
      }
      return false;
    }
    renderRoomState(normalized);
    publishedRoomTransition = normalized.transitionId;
    publishedRoomFingerprint = fingerprint;
    return true;
  }

  function renderRoomState(next) {
    if (!room || (Number(next.transitionId) < Number(room.transitionId || 0))) return;
    const priorInviteGeneration = Number(room.inviteGeneration);
    const publishedInviteGeneration = Number(next.inviteGeneration);
    const invalidatesDisplayedInvite = inviteActive &&
      ((Number.isInteger(publishedInviteGeneration) &&
        publishedInviteGeneration !== priorInviteGeneration) ||
       next.phase === "closed");
    room = {...room, ...next};
    if (invalidatesDisplayedInvite) clearInvitePresentation(false);
    const controllers = activeControllers();
    if (removeControllerId && !controllers.some((controller) =>
        controller.controllerId === removeControllerId)) {
      const wasConfirmingRemoval = removeDialog.open;
      const returnTarget = removeReturnFocus?.closest("li");
      removeControllerId = "";
      removeReturnFocus = null;
      if (removeDialog.open) removeDialog.close();
      if (dialog.open && returnTarget?.isConnected) {
        returnTarget.focus({preventScroll: true});
      }
      if (wasConfirmingRemoval) {
        announce("That phone was already removed. Its controls are neutral.");
      }
    }
    syncRemoteReservations(controllers);
    const pending = controllers.filter((controller) => controller.phase === "pending");
    const pendingList = $("party-pending-list");
    pendingList.replaceChildren();
    $("party-pending-empty").hidden = pending.length !== 0;
    // The phrase-comparison instruction is the host half of the MITM defense:
    // showing the phrase is decorative unless the operator is told to compare
    // it and approve only on an exact match. Mirrors the native launcher.
    $("party-pending-instruction").hidden = pending.length === 0;
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
      const remove = tile.querySelector(".party-seat-remove");
      remove.hidden = !controller;
      remove.disabled = false;
      remove.onclick = controller ? () => confirmRemove(controller, remove) : null;
      remove.setAttribute("aria-label", controller
        ? `Remove ${controller.name || "phone controller"} from Controller ${seat}`
        : `No phone assigned to Controller ${seat}`);
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

  function clearRoomReconnectTimers() {
    if (socketReconnectTimer !== null) clearTimeout(socketReconnectTimer);
    if (socketStableTimer !== null) clearTimeout(socketStableTimer);
    socketReconnectTimer = null;
    socketStableTimer = null;
  }

  function scheduleRoomReconnect(generation) {
    if (!room || leavingPage || generation !== socketGeneration ||
        socketReconnectTimer !== null) return;
    if (socketReconnectAttempt >= 5) {
      socketReconnectExhausted = true;
      $("party-error-message").textContent =
        "The controller-room connection is paused. Approved phones may keep " +
        "working directly; new pairings wait until you try again.";
      if (dialog.open) {
        show("error");
        $("party-retry").focus();
      }
      announce("Controller room paused after five reconnect attempts. Try again when ready.");
      return;
    }
    const delay = Math.min(4800, 300 * (2 ** Math.min(socketReconnectAttempt++, 4)));
    socketReconnectTimer = setTimeout(() => {
      socketReconnectTimer = null;
      if (generation === socketGeneration) connectRoom();
    }, delay);
  }

  function failRoomSocket(connection, generation, code, reason) {
    if (connection !== socket || generation !== socketGeneration) return;
    socket = null;
    if (socketStableTimer !== null) clearTimeout(socketStableTimer);
    socketStableTimer = null;
    try { connection.close(code, reason); } catch (_) {}
    announce("Controller room reconnecting… Approved phone controls remain direct.");
    scheduleRoomReconnect(generation);
  }

  function failRoomSocketSetup(generation) {
    if (generation !== socketGeneration) return;
    socket = null;
    if (socketStableTimer !== null) clearTimeout(socketStableTimer);
    socketStableTimer = null;
    announce("Controller room reconnecting… Approved phone controls remain direct.");
    scheduleRoomReconnect(generation);
  }

  function applyTerminalRoomClose(reason) {
    clearInvitePresentation(false);
    removeControllerId = "";
    removeReturnFocus = null;
    endReturnFocus = null;
    if (removeDialog.open) removeDialog.close();
    if (endDialog.open) endDialog.close();
    room = null;
    inviteRevocation = null;
    ++operation;
    clearRoomReconnectTimers();
    socketGeneration++;
    socketReconnectAttempt = 0;
    socketReconnectExhausted = false;
    publishedRoomTransition = 0;
    publishedRoomFingerprint = "";
    for (const controllerId of [...peers.keys()]) retirePeer(controllerId, true);
    resetRemotePads();
    signaledControllers.clear();
    pairingPhrases.clear();
    phraseTasks.clear();
    hostIdentity = null;
    setOverlayOpen(false);
    const expired = reason === "room_expired";
    $("party-error-message").textContent = expired
      ? "This controller room reached its 24-hour limit. Open a new room to pair phones again."
      : "This controller room ended. Open a new room to pair phones again.";
    if (dialog.open) {
      show("error");
      $("party-retry").focus();
    }
    announce(expired ? "Controller room expired. Phone controls were released."
      : "Controller room ended. Phone controls were released.");
  }

  function connectRoom() {
    if (testConfig) {
      if (testConfig.initialRoomState) renderRoomState(testConfig.initialRoomState);
      return true;
    }
    if (!room || leavingPage) return false;
    clearRoomReconnectTimers();
    const previous = socket;
    socket = null;
    socketGeneration++;
    const generation = socketGeneration;
    try { previous?.close?.(1000, "controller_socket_replaced"); } catch (_) {}
    let connection;
    try {
      const url = new URL(serviceUrl(`/api/party/${room.roomId}/connect`));
      url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
      connection = new WebSocket(url,
        ["gb-control-v1", `gb-host.${room.hostCredential}`]);
      socket = connection;
    } catch (_) {
      failRoomSocketSetup(generation);
      return false;
    }
    try {
      connection.addEventListener("open", () => {
        if (connection !== socket || generation !== socketGeneration) return;
        socketReconnectExhausted = false;
        if (dialog.open) show("room");
        announce("Controller room connected.");
        socketStableTimer = setTimeout(() => {
          socketStableTimer = null;
          if (connection === socket && generation === socketGeneration) {
            socketReconnectAttempt = 0;
          }
        }, 30_000);
        for (const [controllerId, peer] of peers) {
          if (!peer.remoteReady && peer.pc.signalingState === "have-local-offer" &&
              peer.pc.localDescription) {
            sendPeerOffer(controllerId, peer);
          } else if (peer.pc.connectionState !== "connected") {
            scheduleIceRestart(controllerId, peer, 0);
          }
        }
        for (const controller of activeControllers()) {
          void ensurePeer(controller.controllerId);
        }
      });
      connection.addEventListener("message", (event) => {
        if (connection !== socket || generation !== socketGeneration) return;
        if (typeof event.data !== "string" || event.data.length > 64 * 1024 ||
            new TextEncoder().encode(event.data).byteLength > 64 * 1024) {
          failRoomSocket(connection, generation, 4009, "invalid_party_message");
          return;
        }
        try {
          const value = JSON.parse(event.data);
          if (value?.type === "room_state") applyPublishedRoomState(value);
          else void handleSignal(value);
        } catch (_) {
          failRoomSocket(connection, generation, 4003, "invalid_party_message");
        }
      });
      connection.addEventListener("error", () => {
        failRoomSocket(connection, generation, 4011, "controller_socket_error");
      });
      connection.addEventListener("close", (event) => {
        if (socket !== connection || generation !== socketGeneration) return;
        socket = null;
        if (socketStableTimer !== null) clearTimeout(socketStableTimer);
        socketStableTimer = null;
        if (!room || leavingPage) return;
        if (event.code === 4000 &&
            ["host_closed", "room_expired"].includes(event.reason)) {
          applyTerminalRoomClose(event.reason);
          return;
        }
        announce("Controller room reconnecting… Phone controls remain safe.");
        scheduleRoomReconnect(generation);
      });
    } catch (_) {
      failRoomSocket(connection, generation, 4011,
        "controller_socket_setup_failed");
      return false;
    }
    return true;
  }

  async function openRoom() {
    const generation = ++operation;
    /* A terminal room may have left an HTTP revoke in flight. It belongs to
     * the old capability generation and must never delay or annotate the next
     * independently-created room. Its completion handlers are room-bound. */
    inviteRevocation = null;
    show("opening");
    try {
      hostIdentity = await globalThis.MDKRPartySas.createIdentity();
      const created = await request("/api/party/create", {
        pageBound: true,
        headers: {"content-type": "application/json"},
        body: {hostPublicKey: hostIdentity.publicKey},
      });
      if (generation !== operation || !dialog.open) return;
      renderInvite(created);
      room.controllers = [];
      room.transitionId = 0;
      publishedRoomTransition = 0;
      publishedRoomFingerprint = "";
      show("room");
      if (!connectRoom()) {
        show("opening");
        announce("Private controller room opened. Reconnecting before showing the invite…");
        return;
      }
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
    if (source) source.disabled = true;
    try {
      await request(`/api/party/${room.roomId}/${action}`, {
        headers: {"content-type": "application/json",
          authorization: `Bearer ${room.hostCredential}`},
        body: {controllerId, ...(value && typeof value === "object" ? value : {})},
      });
      announce(action === "approve" ? "Phone approved." :
        action === "remove" ? "Phone removed. Its controls are now neutral." :
        "Phone declined.");
    } catch (error) {
      if (source?.isConnected) source.disabled = false;
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
      const pendingRevocation = inviteRevocation;
      if (pendingRevocation) await pendingRevocation;
      if (generation !== operation || !dialog.open || !room) return;
      const expectedInviteGeneration = Number(room.inviteGeneration);
      const value = await request(`/api/party/${room.roomId}/rotate`, {
        pageBound: true,
        headers: {"content-type": "application/json",
          authorization: `Bearer ${room.hostCredential}`},
        body: {expectedInviteGeneration},
      });
      if (generation !== operation || !dialog.open) return;
      renderInvite(value, expectedInviteGeneration);
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
    abortPageBoundRequests();
    setOverlayOpen(false);
    if (removeDialog.open) removeDialog.close();
    clearInvitePresentation(false);
    const current = room;
    if (current) {
      const expectedGeneration = Number(current.inviteGeneration) + 1;
      const task = request(`/api/party/${current.roomId}/revoke`, {
        headers: {"content-type": "application/json",
          authorization: `Bearer ${current.hostCredential}`}, body: {},
      }).then((value) => {
        const normalized = normalizedRevocation(value, expectedGeneration);
        if (!normalized) throw new Error("service_unavailable");
        if (room?.roomId === current.roomId &&
            Number(room.inviteGeneration) <= normalized.inviteGeneration) {
          room = {...room, inviteGeneration: normalized.inviteGeneration};
        }
      }).catch(() => {
        if (room?.roomId === current.roomId) announce(
          "The invite could not be revoked immediately; it still expires within two minutes."
        );
      }).finally(() => {
        if (inviteRevocation === task) inviteRevocation = null;
      });
      inviteRevocation = task;
    }
    closeDialog();
  }

  async function endRoom() {
    if (testState) testState.lifecycle.push("endRoom");
    clearInvitePresentation(false);
    const ending = room;
    room = null;
    inviteRevocation = null;
    if (removeDialog.open) removeDialog.close();
    setOverlayOpen(false);
    ++operation;
    abortPageBoundRequests();
    if (socket) { socket.close(1000, "host_closed"); socket = null; }
    if (socketReconnectTimer !== null) {
      clearTimeout(socketReconnectTimer); socketReconnectTimer = null;
    }
    if (socketStableTimer !== null) {
      clearTimeout(socketStableTimer); socketStableTimer = null;
    }
    socketGeneration++;
    socketReconnectAttempt = 0;
    socketReconnectExhausted = false;
    publishedRoomTransition = 0;
    publishedRoomFingerprint = "";
    for (const controllerId of [...peers.keys()]) retirePeer(controllerId, true);
    resetRemotePads();
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

  function confirmRemove(controller, source) {
    if (!controller || removeDialog.open) return;
    removeControllerId = controller.controllerId;
    removeReturnFocus = source;
    $("party-remove-copy").textContent =
      `${controller.name || "This phone"} will stop controlling Controller ` +
      `${controller.seat}. Its input becomes neutral and the seat becomes available.`;
    removeDialog.showModal();
    requestAnimationFrame(() => $("party-remove-cancel").focus());
  }

  function updateCountdown() {
    if (!room) return;
    const remaining = Math.max(0, deadline - Date.now());
    if (remaining === 0) {
      if (inviteActive) clearInvitePresentation(true, true);
      return;
    }
    const seconds = Math.ceil(remaining / 1000);
    $("party-expiry").textContent = remaining
      ? `Invite expires in ${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`
      : "Invite expired — extend to add another phone";
    $("party-extend").textContent = remaining ? "Extend 2 minutes" : "Show a new QR code";
  }

  function setRomReady(ready) {
    romReady = Boolean(ready);
    trigger.disabled = !surfaceEnabled || !romReady;
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
    if (!surfaceEnabled) return;
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
    if (room && socketReconnectExhausted) {
      socketReconnectAttempt = 0;
      socketReconnectExhausted = false;
      show("opening");
      connectRoom();
    } else if (room) void extendInvite();
    else void openRoom();
  });
  $("party-extend").addEventListener("click", () => void extendInvite());
  $("party-close").addEventListener("click", dismissRoom);
  $("party-end").addEventListener("click", confirmEndRoom);
  $("party-remove-cancel").addEventListener("click", () => removeDialog.close());
  $("party-remove-confirm").addEventListener("click", () => {
    const controllerId = removeControllerId;
    const source = removeReturnFocus;
    const returnTarget = source?.closest("li");
    removeControllerId = "";
    removeReturnFocus = null;
    removeDialog.close();
    if (dialog.open && returnTarget?.isConnected) {
      returnTarget.focus({preventScroll: true});
    }
    if (controllerId) void control("remove", controllerId, source);
  });
  removeDialog.addEventListener("close", () => {
    if (removeReturnFocus?.isConnected && !removeReturnFocus.hidden && dialog.open) {
      removeReturnFocus.focus({preventScroll: true});
    }
    removeReturnFocus = null;
    removeControllerId = "";
  });
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
    /* Starting local play closes invite custody, not the approved controller
     * room. Reuse the same immediate erase + generation-correlated revoke as
     * Close/Escape before handing focus to the game. */
    dismissRoom();
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
    if (testState) testState.lifecycle.push("pagehide");
    leavingPage = true;
    ++operation;
    abortPageBoundRequests();
    clearInvitePresentation(false);
    if (timer !== null) clearInterval(timer);
    timer = null;
    clearRoomReconnectTimers();
    socketGeneration++;
    try { socket?.close(1000, "pagehide"); } catch (_) {}
    socket = null;
    for (const controllerId of [...peers.keys()]) retirePeer(controllerId, false);
    signaledControllers.clear();
    resetRemotePads();
  });
  addEventListener("pageshow", (event) => {
    if (!event.persisted) return;
    if (testState) testState.lifecycle.push("pageshow:persisted");
    leavingPage = false;
    if (timer === null) timer = setInterval(updateCountdown, 1000);
    if (room) {
      connectRoom();
      if (dialog.open) void extendInvite();
    } else if (dialog.open) {
      void openRoom();
    }
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
      testPingControl: () => {
        const entry = peers.entries().next().value;
        if (!entry) return false;
        scheduleControlPing(entry[0], entry[1], 0);
        return true;
      },
      testExpireControl: () => {
        const entry = peers.entries().next().value;
        if (!entry) return false;
        if (entry[1].pingTimer !== null) clearTimeout(entry[1].pingTimer);
        entry[1].pingTimer = null;
        entry[1].pingOutstandingAt = Date.now() - 15000;
        scheduleControlPing(entry[0], entry[1], 0);
        return true;
      },
      testPeerState: () => {
        const entry = peers.values().next().value;
        return entry ? {generation: entry.generation,
          connectionState: entry.pc.connectionState,
          signalingState: entry.pc.signalingState,
          state: entry.state?.readyState, control: entry.control?.readyState} : null;
      },
    } : {}),
  });
})();
