"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const testConfig = globalThis.__mdkrControllerTestConfig &&
    typeof globalThis.__mdkrControllerTestConfig === "object"
    ? globalThis.__mdkrControllerTestConfig : null;
  const testState = testConfig ? {
    states: [], packets: [], neutralizations: 0, requests: [], errors: [],
  } : null;
  if (testState) globalThis.__mdkrControllerTestState = testState;

  const views = {
    opening: $("state-opening"), code: $("state-code"), waiting: $("state-waiting"),
    assigned: $("state-assigned"), controller: $("state-controller"),
    reconnecting: $("state-reconnecting"), duplicate: $("state-duplicate"),
    error: $("state-error"),
  };
  const pad = {buttons: 0, stickX: 0, stickY: 0};
  let phase = "opening";
  let seat = 0;
  let connectionSequence = 1;
  let sampleSequence = 0;
  let padHistory = [];
  let active = false;
  let inputTestPassed = false;
  let wakeLock = null;
  let surface = null;
  let heartbeat = null;
  let capability = "";
  let releaseTabLease = null;
  let transport = null;
  let receiveTestSignal = null;
  let leavingPage = false;
  let reconnectResume = "controller";
  let leaveReturnFocus = null;

  const errors = {
    missing_link: ["Controller link missing",
      "Scan the QR on the display again or enter its current room code."],
    embedded: ["Open in Safari or Chrome",
      "This in-app browser cannot make a reliable direct controller connection."],
    unsupported: ["Browser update needed",
      "This controller needs WebRTC and Pointer Events. Update Safari or Chrome and try again."],
    update_required: ["Controller update required",
      "The display uses a newer controller protocol. Refresh this controller."],
    invite_expired: ["Invite expired",
      "Ask the display to show a new QR code, then scan it again."],
    left_room: ["Controller disconnected",
      "This phone no longer controls a racer. Enter the newest code to join again."],
    invite_rotated: ["Invite replaced",
      "The display replaced that invite. Scan its newest QR code."],
    room_full: ["Controller room full",
      "Four controllers are already approved. Ask the display to free a seat."],
    pending_full: ["Too many phones waiting",
      "Ask the display to approve or decline a waiting phone, then try again."],
    approval_rejected: ["Phone not approved",
      "The display declined this phone. Ask the host before trying another code."],
    host_closed: ["Controller room ended",
      "The display ended this phone controller room."],
    room_expired: ["Controller room ended",
      "This phone controller room reached its time limit."],
    duplicate_controller: ["Controller already joined",
      "This phone already has an active controller connection."],
    seat_reclaimed: ["Controller seat reassigned",
      "The display reassigned this controller seat. Join again if needed."],
    rate_limited: ["Too many code attempts",
      "Wait a few minutes, then enter the newest code from the display."],
    service_budget_safe: ["Phone pairing is full right now",
      "Keyboard, gamepads and this display’s touch controls still work offline."],
    service_unavailable: ["Pairing unavailable",
      "The controller service could not be reached. Check the internet connection or play with the display’s keyboard or gamepads."],
  };

  function announce(message) { $("live-status").textContent = message; }

  function setConnection(kind, label) {
    const mark = $("connection-mark");
    mark.className = "connection-mark" + (kind ? " " + kind : "");
    mark.setAttribute("aria-label", label);
  }

  function render(next, options = {}) {
    phase = next;
    Object.entries(views).forEach(([name, element]) => {
      element.hidden = name !== next;
    });
    if (testState) testState.states.push(next);
    if (next === "controller") {
      setConnection("connected", "Controller connected");
      announce("Controller connected. Controller " + seat + ".");
      requestAnimationFrame(() => $("phone-touch-stick").focus?.());
    } else if (next === "reconnecting" || next === "error" || next === "duplicate") {
      setConnection("problem", next === "reconnecting" ? "Reconnecting" : "Not connected");
    } else {
      setConnection("", "Connecting");
    }
    if (options.focus) requestAnimationFrame(() => options.focus.focus());
  }

  function showError(id, recoveryLabel = "Enter another code") {
    neutralize("error");
    const copy = errors[id] || ["Couldn’t join", "Try the current QR or room code again."];
    $("error-title").textContent = copy[0];
    $("error-message").textContent = copy[1];
    $("error-recovery").textContent = recoveryLabel;
    $("copy-link").hidden = id !== "embedded";
    render("error", {focus: $("error-recovery")});
  }

  function bytesFromCapability(value) {
    if (!/^[A-Za-z0-9_-]{22,256}$/.test(value)) return null;
    try {
      const base64 = value.replace(/-/g, "+").replace(/_/g, "/") +
        "===".slice((value.length + 3) % 4);
      const binary = atob(base64);
      if (binary.length < 16 || binary.length > 192) return null;
      return Uint8Array.from(binary, (character) => character.charCodeAt(0));
    } catch (_) { return null; }
  }

  function consumeFragment() {
    const raw = location.hash.startsWith("#") ? location.hash.slice(1) : "";
    // Remove the bearer before any feature probe, network operation, referrer
    // opportunity or error report. It remains only in this closure.
    globalThis.history.replaceState(null, "", location.pathname + location.search);
    if (testConfig) return testConfig.codeMode
      ? "" : (testConfig.capability || "test-controller-capability");
    return bytesFromCapability(raw) ? raw : "";
  }

  async function tabKey(value) {
    if (globalThis.crypto && globalThis.crypto.subtle && globalThis.TextEncoder) {
      const digest = await globalThis.crypto.subtle.digest(
        "SHA-256", new TextEncoder().encode(value));
      return [...new Uint8Array(digest).slice(0, 12)]
        .map((byte) => byte.toString(16).padStart(2, "0")).join("");
    }
    return "session"; // old browser fallback is intentionally room-scoped only
  }

  async function acquireTabLease(value, force = false) {
    if (!navigator.locks || typeof navigator.locks.request !== "function") return true;
    const name = "golden-balloon-controller-" + await tabKey(value);
    return new Promise((resolve) => {
      let settled = false;
      navigator.locks.request(
        name, force ? {steal: true} : {ifAvailable: true}, async (lock) => {
        if (!lock) { settled = true; resolve(false); return; }
        if (!settled) { settled = true; resolve(true); }
        await new Promise((release) => { releaseTabLease = release; });
        }).catch(() => { if (!settled) resolve(true); });
    });
  }

  function packetForCurrent() {
    const prior = padHistory.slice(0, -1).slice(-8);
    const edges = prior.map((entry) => ({
      sequenceDelta: (sampleSequence - entry.sequence) >>> 0,
      buttons: entry.buttons,
      stickX: entry.stickX,
      stickY: entry.stickY,
    })).filter((edge) => edge.sequenceDelta > 0 && edge.sequenceDelta <= 127);
    let flags = 1;
    if (pad.buttons === 0 && pad.stickX === 0 && pad.stickY === 0) flags |= 2;
    if (edges.length) flags |= 4;
    return globalThis.MDKRPartyProtocol.encode({
      flags, connectionSequence, sampleSequence,
      senderTimeMs: Math.floor(performance.now()) >>> 0,
      buttons: pad.buttons >>> 0, stickX: pad.stickX | 0, stickY: pad.stickY | 0,
      edges,
    });
  }

  function publishPad(force = false) {
    if (!active && !force) return;
    const previous = padHistory[padHistory.length - 1];
    const changed = !previous || previous.buttons !== pad.buttons ||
      previous.stickX !== pad.stickX || previous.stickY !== pad.stickY;
    if (changed) {
      sampleSequence = (sampleSequence + 1) >>> 0;
      padHistory.push({sequence: sampleSequence, buttons: pad.buttons >>> 0,
        stickX: pad.stickX | 0, stickY: pad.stickY | 0});
      if (padHistory.length > 9) padHistory.shift();
    }
    if (!changed && !force) return;
    const bytes = packetForCurrent();
    if (transport && transport.sendState) transport.sendState(bytes);
    if (testState) {
      testState.packets.push({
        hex: [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join(""),
        decoded: globalThis.MDKRPartyProtocol.decode(bytes),
      });
    }
  }

  function neutralize(reason) {
    const wasActive = active;
    if (surface) surface.releaseAll();
    pad.buttons = 0; pad.stickX = 0; pad.stickY = 0;
    if (wasActive) publishPad(true);
    active = false;
    if (testState) testState.neutralizations++;
    if (transport && transport.neutral) transport.neutral(reason);
  }

  function mockTransport() {
    return {
      sendState() {}, neutral() {}, inputTest() { return false; },
      async redeem() {
        if (testConfig.redeemError) throw new Error(testConfig.redeemError);
        return {phrase: testConfig.phrase || "Bright Balloon",
          protocol: testConfig.protocol || 1};
      },
      async redeemCode() {
        if (testConfig.redeemError) throw new Error(testConfig.redeemError);
        return {phrase: testConfig.phrase || "Bright Balloon",
          protocol: testConfig.protocol || 1};
      },
    };
  }

  function networkTransport() {
    let controlSocket = null;
    let peer = null;
    let stateChannel = null;
    let controlChannel = null;
    let currentPeerGeneration = 0;
    let terminalPeerGeneration = 0;
    let recoveryPeerGeneration = 0;
    let controllerInfo = null;
    let inputTestNonce = 0;
    let controlGeneration = 0;
    let reconnectTimer = null;
    let reconnectAttempt = 0;
    let signalingLimited = false;
    const wiredPeers = new WeakSet();
    const identityPromise = globalThis.MDKRPartySas.createIdentity();

    function directChannelsOpen() {
      return peer?.connectionState === "connected" &&
        stateChannel?.readyState === "open" && controlChannel?.readyState === "open";
    }

    function completeDirectRecovery(connection) {
      if (peer !== connection || !directChannelsOpen()) return;
      if (phase === "reconnecting" &&
          currentPeerGeneration >= recoveryPeerGeneration) {
        recoveryPeerGeneration = 0;
        reconnectComplete(connectionSequence);
      }
    }

    function directTransportLost(connection, terminal = false) {
      if (peer !== connection || leavingPage) return;
      if (terminal) {
        if (terminalPeerGeneration === currentPeerGeneration) return;
        terminalPeerGeneration = currentPeerGeneration;
        recoveryPeerGeneration = currentPeerGeneration + 1;
        peer = null;
        stateChannel = null;
        controlChannel = null;
        connection.close();
      } else {
        recoveryPeerGeneration = currentPeerGeneration;
      }
      signalingLimited = false;
      if (["assigned", "controller"].includes(phase)) reconnect(1);
    }

    async function post(path, body) {
      if (testConfig?.request) return testConfig.request(path, body);
      const response = await fetch(path, {
        method: "POST", headers: {"content-type": "application/json"},
        cache: "no-store", credentials: "omit", body: JSON.stringify(body),
      });
      if (testState) testState.requests.push(response.url);
      if (!response.ok) {
        const value = await response.json().catch(() => ({}));
        throw new Error(value.error || "service_unavailable");
      }
      return response.json();
    }

    function signal(value) {
      if (testConfig?.directSignaling) {
        testState.signals ||= [];
        const encodedValue = {...value, controllerId: controllerInfo.controllerId};
        testState.signals.push(JSON.parse(JSON.stringify(encodedValue)));
        if (typeof testConfig.sendSignal === "function") testConfig.sendSignal(encodedValue);
        return true;
      }
      if (controlSocket?.readyState !== WebSocket.OPEN) return false;
      const encoded = JSON.stringify({...value, controllerId: controllerInfo.controllerId});
      if (encoded.length > 60 * 1024) return false;
      controlSocket.send(encoded);
      return true;
    }

    async function acceptOffer(message) {
      if (message.to !== controllerInfo.controllerId || !message.sdp ||
          JSON.stringify(message.sdp).length > 60 * 1024 ||
          !Number.isSafeInteger(Number(message.peerGeneration)) ||
          Number(message.peerGeneration) <= 0) return;
      const offeredGeneration = Number(message.peerGeneration);
      if (offeredGeneration < currentPeerGeneration ||
          offeredGeneration === terminalPeerGeneration) return;
      let connection = peer;
      if (!connection || offeredGeneration !== currentPeerGeneration ||
          connection.signalingState === "closed") {
        if (connection) {
          peer = null;
          connection.close();
        }
        stateChannel = null;
        controlChannel = null;
        currentPeerGeneration = offeredGeneration;
        connection = new RTCPeerConnection({iceServers: [{
          urls: "stun:stun.cloudflare.com:3478",
        }]});
        peer = connection;
      } else if (connection.signalingState !== "stable") {
        return;
      }
      if (!wiredPeers.has(connection)) {
        wiredPeers.add(connection);
        connection.addEventListener("icecandidate", (event) => {
        if (peer !== connection || currentPeerGeneration !== offeredGeneration) return;
        if (event.candidate) signal({type: "webrtc_ice",
          peerGeneration: offeredGeneration,
          candidate: event.candidate.toJSON()});
        });
        connection.addEventListener("connectionstatechange", () => {
        if (peer !== connection) return;
        if (connection.connectionState === "connected") {
          completeDirectRecovery(connection);
        } else if (["failed", "disconnected"].includes(connection.connectionState)) {
          directTransportLost(connection);
        } else if (connection.connectionState === "closed") {
          directTransportLost(connection, true);
        }
        });
        connection.addEventListener("datachannel", (event) => {
        if (peer !== connection) { event.channel.close(); return; }
        if (event.channel.label === "mdkr-pad-state-v1") {
          stateChannel = event.channel;
          stateChannel.binaryType = "arraybuffer";
          stateChannel.addEventListener("open", () => completeDirectRecovery(connection));
          stateChannel.addEventListener("close", () => {
            if (stateChannel === event.channel) directTransportLost(connection, true);
          });
          stateChannel.addEventListener("error", () => {
            if (stateChannel === event.channel) directTransportLost(connection, true);
          });
        } else if (event.channel.label === "mdkr-pad-control-v1") {
          controlChannel = event.channel;
          controlChannel.addEventListener("open", () => {
            if (controlChannel !== event.channel) return;
            controlChannel.send(JSON.stringify({
              type: "controller_ready", protocol: 1,
              controllerId: controllerInfo.controllerId,
              connectionSequence,
              capabilities: {vibration: typeof navigator.vibrate === "function"},
            }));
            completeDirectRecovery(connection);
          });
          controlChannel.addEventListener("close", () => {
            if (controlChannel === event.channel) directTransportLost(connection, true);
          });
          controlChannel.addEventListener("error", () => {
            if (controlChannel === event.channel) directTransportLost(connection, true);
          });
          controlChannel.addEventListener("message", (controlEvent) => {
            if (typeof controlEvent.data !== "string" || controlEvent.data.length > 4096) return;
            try {
              const value = JSON.parse(controlEvent.data);
              if (value.type === "input_test_ack" && value.nonce === inputTestNonce) {
                markInputTestPassed();
              } else if (value.type === "rumble" && value.protocol === 1 && active &&
                         typeof navigator.vibrate === "function") {
                const strength = Math.max(0, Math.min(65535,
                  Number(value.strength) || 0));
                const duration = Math.max(0, Math.min(250,
                  Number(value.durationMs) || 0));
                if ($("haptics").checked) navigator.vibrate(strength > 0 ? duration : 0);
              }
            } catch (_) { /* malformed direct control message is ignored */ }
          });
        }
        });
      }
      try {
        await connection.setRemoteDescription(message.sdp);
        await connection.setLocalDescription(await connection.createAnswer());
        signal({type: "webrtc_answer", peerGeneration: offeredGeneration,
          sdp: connection.localDescription});
      } catch (_) {
        if (peer === connection) directTransportLost(connection, true);
      }
    }

    function connectControl(value) {
      if (!value?.roomId || !value?.credential) return;
      controllerInfo = value;
      const handleMessage = (update) => {
        if (update.type === "controller_state" &&
            ["approved", "leased", "connected"].includes(update.phase)) {
          const nextSequence = Number(update.connectionSequence) >>> 0;
          const sequenceChanged = connectionSequence !== 0 &&
            nextSequence !== connectionSequence;
          const wasSignalingLimited = signalingLimited;
          signalingLimited = false;
          connectionSequence = nextSequence;
          if (phase === "waiting") approve(update.seat, value.phrase);
          else if (sequenceChanged && directChannelsOpen()) {
            recoveryPeerGeneration = currentPeerGeneration + 1;
            reconnect(1);
          } else if (phase === "reconnecting" && directChannelsOpen()) {
            recoveryPeerGeneration = currentPeerGeneration;
            reconnectComplete(connectionSequence);
          } else if (wasSignalingLimited && directChannelsOpen() &&
                     ["assigned", "controller"].includes(phase)) {
            setConnection("connected", "Controller connected");
            announce("Room service reconnected. Direct controls stayed connected.");
          }
          signal({type: "controller_hello"});
        } else if (update.type === "controller_state" && update.phase === "closed") {
          showError("approval_rejected");
        } else if (update.type === "webrtc_offer") void acceptOffer(update);
        else if (update.type === "webrtc_ice" && update.to === value.controllerId &&
                 Number.isSafeInteger(update.peerGeneration) &&
                 Number(update.peerGeneration) === currentPeerGeneration &&
                 currentPeerGeneration !== terminalPeerGeneration &&
                 update.candidate && JSON.stringify(update.candidate).length <= 4096) {
          void peer?.addIceCandidate(update.candidate).catch(() => {});
        }
      };
      receiveTestSignal = handleMessage;
      if (testConfig?.directSignaling) {
        setTimeout(() => {
          handleMessage({type: "controller_state", phase: "leased",
            seat: testConfig.seat || 1,
            connectionSequence: testConfig.connectionSequence || 1});
          signal({type: "controller_hello"});
        }, 0);
        return;
      }
      const url = new URL(`/api/party/${value.roomId}/connect`, location.origin);
      url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
      const generation = ++controlGeneration;
      const connection = new WebSocket(url, ["gb-control-v1",
        `gb-controller.${value.credential}`]);
      controlSocket = connection;
      connection.binaryType = "arraybuffer";
      connection.addEventListener("open", () => {
        reconnectAttempt = 0;
        if (reconnectTimer !== null) { clearTimeout(reconnectTimer); reconnectTimer = null; }
        signal({type: "controller_hello"});
      });
      connection.addEventListener("message", (event) => {
        if (typeof event.data !== "string") return;
        try {
          const update = JSON.parse(event.data);
          handleMessage(update);
        } catch (_) { /* malformed signaling cannot mutate controller state */ }
      });
      connection.addEventListener("close", () => {
        if (generation !== controlGeneration || leavingPage) return;
        reconnectAttempt++;
        if (directChannelsOpen() && (phase === "assigned" || phase === "controller")) {
          if (!signalingLimited) {
            signalingLimited = true;
            setConnection("limited",
              "Controller connected directly; room service reconnecting");
            announce("Controller remains connected directly. Room service reconnecting.");
          }
        } else {
          signalingLimited = false;
          reconnect(reconnectAttempt);
        }
        const delay = Math.min(8000,
          300 * (2 ** Math.min(5, reconnectAttempt - 1)));
        reconnectTimer = setTimeout(() => connectControl(value), delay);
      });
    }

    const api = {
      sendState(bytes) {
        if (stateChannel?.readyState !== "open" || stateChannel.bufferedAmount > 4096) return;
        stateChannel.send(bytes);
      },
      neutral() {},
      inputTest(pressed) {
        if (!pressed || controlChannel?.readyState !== "open") return true;
        inputTestNonce = (inputTestNonce + 1) >>> 0;
        controlChannel.send(JSON.stringify({type: "input_test", nonce: inputTestNonce}));
        return true;
      },
      close() {
        signalingLimited = false;
        controlGeneration++;
        if (reconnectTimer !== null) { clearTimeout(reconnectTimer); reconnectTimer = null; }
        if (controlSocket) { controlSocket.close(1000, "controller_left"); controlSocket = null; }
        if (peer) { peer.close(); peer = null; }
        stateChannel = null;
        controlChannel = null;
        controllerInfo = null;
      },
      async redeem(secret) {
        const identity = await identityPromise;
        const value = await post("/api/controller/redeem", {capability: secret,
          protocol: 1, name: $("device-name").value,
          controllerPublicKey: identity.publicKey});
        if (value.hostPublicKey) value.phrase = await globalThis.MDKRPartySas.phrase(
          identity.privateKey, value.hostPublicKey, {roomId: value.roomId,
            hostPublicKey: value.hostPublicKey,
            controllerPublicKey: identity.publicKey});
        connectControl(value);
        return value;
      },
      async redeemCode(code) {
        const identity = await identityPromise;
        const value = await post("/api/controller/code", {code, protocol: 1,
          name: $("device-name").value, controllerPublicKey: identity.publicKey});
        if (value.hostPublicKey) value.phrase = await globalThis.MDKRPartySas.phrase(
          identity.privateKey, value.hostPublicKey, {roomId: value.roomId,
            hostPublicKey: value.hostPublicKey,
            controllerPublicKey: identity.publicKey});
        connectControl(value);
        return value;
      },
    };
    return api;
  }

  async function joinCode(rawCode) {
    const code = String(rawCode || "").replace(/\D/g, "");
    if (code.length !== 6) {
      $("code-error").textContent = "Enter all six digits from the display.";
      $("code-error").hidden = false;
      $("room-code").setAttribute("aria-invalid", "true");
      $("room-code").focus();
      return;
    }
    $("code-error").hidden = true;
    $("room-code").setAttribute("aria-invalid", "false");
    $("join-code").disabled = true;
    render("opening");
    try {
      const result = await transport.redeemCode(code);
      if (result.protocol !== 1) { showError("update_required", "Refresh controller"); return; }
      $("pairing-phrase").textContent = result.phrase || "Bright Balloon";
      render("waiting", {focus: $("device-name")});
      if (testConfig && testConfig.autoApprove !== false) {
        setTimeout(() => approve(testConfig.seat || 1), testConfig.approveDelayMs || 0);
      }
    } catch (error) {
      if (error?.message && errors[error.message]) {
        showError(error.message);
        return;
      }
      render("code", {focus: $("room-code")});
      $("code-error").textContent = error?.message === "invalid_code"
        ? "That code is invalid or expired. Check the display and try again."
        : "Could not reach the controller room. Check your connection and try again.";
      $("code-error").hidden = false;
      $("room-code").setAttribute("aria-invalid", "true");
    } finally {
      $("join-code").disabled = false;
    }
  }

  function approve(controllerSeat = 1, phrase = null) {
    if (phase !== "waiting") return false;
    seat = Math.max(1, Math.min(4, Number(controllerSeat) || 1));
    if (phrase) $("pairing-phrase").textContent = phrase;
    $("seat-badge").textContent = String(seat);
    $("assigned-title").textContent = "Controller " + seat;
    $("controller-seat").textContent = "Controller " + seat;
    document.documentElement.style.setProperty("--seat",
      ["#4bc7ff", "#ff6f91", "#72e38f", "#c491ff"][seat - 1]);
    render("assigned", {focus: $("input-test")});
    return true;
  }

  function markInputTestPassed() {
    if (inputTestPassed) return;
    inputTestPassed = true;
    $("input-test").classList.add("passed");
    $("input-test-status").textContent = "Connection works";
    $("use-controller").disabled = false;
    announce("Connection works. Use controller is now available.");
  }

  function testPress(pressed) {
    pad.buttons = pressed ? 32768 : 0;
    const waitingForRoundTrip = transport?.inputTest?.(pressed) === true;
    if (pressed && !inputTestPassed) {
      if (!waitingForRoundTrip) markInputTestPassed();
      else $("input-test-status").textContent = "Checking connection…";
    }
  }

  async function requestWakeLock() {
    if (!navigator.wakeLock || !navigator.wakeLock.request) return;
    try {
      wakeLock = await navigator.wakeLock.request("screen");
      $("wake-status").textContent = "Screen will stay awake";
      wakeLock.addEventListener("release", () => {
        wakeLock = null;
        $("wake-status").textContent = "Screen wake lock released";
      }, {once: true});
    } catch (_) {
      $("wake-status").textContent = "Screen wake lock unavailable";
    }
  }

  function useController() {
    if (!inputTestPassed) return;
    active = true;
    padHistory = [];
    pad.buttons = 0; pad.stickX = 0; pad.stickY = 0;
    publishPad(true);
    render("controller");
    void requestWakeLock();
    if (heartbeat === null) heartbeat = setInterval(() => publishPad(true), 50);
  }

  function reconnect(attempt = 1) {
    if (phase !== "reconnecting") reconnectResume = phase;
    neutralize("transport-lost");
    $("reconnect-progress").textContent =
      `Attempt ${Math.max(1, Math.min(5, attempt))} of 5. Controls are safely released.`;
    render("reconnecting", {focus: $("state-reconnecting")});
  }

  function reconnectComplete(epoch) {
    connectionSequence = Number.isInteger(epoch)
      ? epoch >>> 0 : (connectionSequence + 1) >>> 0;
    active = reconnectResume === "controller";
    padHistory = [];
    if (active) {
      publishPad(true);
      render("controller");
    } else {
      render("assigned", {focus: $("input-test")});
    }
  }

  function leave() {
    neutralize("leave");
    transport?.close?.();
    if (heartbeat !== null) { clearInterval(heartbeat); heartbeat = null; }
    if (wakeLock) wakeLock.release().catch(() => {});
    if (releaseTabLease) { releaseTabLease(); releaseTabLease = null; }
    showError("left_room", "Enter another code");
  }

  function confirmLeave(source) {
    if (phase === "opening") { leave(); return; }
    const dialog = $("leave-dialog");
    if (dialog.open) return;
    leaveReturnFocus = source instanceof HTMLElement ? source : null;
    dialog.showModal();
    requestAnimationFrame(() => $("leave-cancel").focus());
  }

  function applySettings() {
    let handedness = "left";
    try {
      handedness = localStorage.getItem("gb-controller-handedness") || "left";
      $("control-size").value = localStorage.getItem("gb-controller-size") || "100";
      $("show-labels").checked = localStorage.getItem("gb-controller-labels") !== "0";
      $("haptics").checked = localStorage.getItem("gb-controller-haptics") !== "0";
    } catch (_) {}
    const radio = document.querySelector(`input[name="handedness"][value="${handedness}"]`);
    if (radio) radio.checked = true;
    $("phone-touch-controls").classList.toggle("right-handed", handedness === "right");
    $("phone-touch-controls").classList.toggle("hide-labels", !$("show-labels").checked);
    $("phone-touch-controls").style.setProperty(
      "--control-scale", String(Number($("control-size").value) / 100));
    if (surface) surface.haptics = $("haptics").checked;
  }

  function saveSettings() {
    const handedness = document.querySelector('input[name="handedness"]:checked').value;
    try {
      localStorage.setItem("gb-controller-handedness", handedness);
      localStorage.setItem("gb-controller-size", $("control-size").value);
      localStorage.setItem("gb-controller-labels", $("show-labels").checked ? "1" : "0");
      localStorage.setItem("gb-controller-haptics", $("haptics").checked ? "1" : "0");
    } catch (_) {}
    applySettings();
  }

  async function start() {
    capability = consumeFragment();
    if (!testConfig) {
      try {
        if (window.top !== window.self) { showError("embedded", "Open in browser"); return; }
      } catch (_) { showError("embedded", "Open in browser"); return; }
      if (!("PointerEvent" in window) || !("RTCPeerConnection" in window) ||
          !globalThis.crypto?.subtle || !globalThis.MDKRPartySas) {
        showError("unsupported", "Refresh controller"); return;
      }
    }
    if (!capability) {
      transport = testConfig ? mockTransport() : networkTransport();
      render("code", {focus: $("room-code")});
      return;
    }
    if (!(await acquireTabLease(capability))) {
      render("duplicate", {focus: $("reclaim-controller")}); return;
    }
    transport = testConfig && !testConfig.directSignaling
      ? mockTransport() : networkTransport();
    try {
      const result = await transport.redeem(capability);
      if (result.protocol !== 1) { showError("update_required", "Refresh controller"); return; }
      $("pairing-phrase").textContent = result.phrase || "Bright Balloon";
      render("waiting", {focus: $("device-name")});
      if (testConfig && testConfig.autoApprove !== false) {
        setTimeout(() => approve(testConfig.seat || 1), testConfig.approveDelayMs || 0);
      }
    } catch (error) {
      const id = error && errors[error.message] ? error.message : "service_unavailable";
      if (testState) testState.errors.push(String(error));
      showError(id);
    }
  }

  surface = new globalThis.MDKRTouchSurface({
    controls: $("phone-touch-controls"), stick: $("phone-touch-stick"),
    knob: $("phone-touch-stick-knob"), state: pad,
    publish: () => publishPad(), clear: () => {
      pad.buttons = 0; pad.stickX = 0; pad.stickY = 0;
    },
  });
  applySettings();

  $("input-test").addEventListener("pointerdown", (event) => {
    event.preventDefault(); testPress(true);
  });
  for (const name of ["pointerup", "pointercancel"]) {
    addEventListener(name, () => testPress(false), true);
  }
  $("input-test").addEventListener("click", (event) => {
    if (event.detail !== 0) return;
    testPress(true); setTimeout(() => testPress(false), 90);
  });
  $("use-controller").addEventListener("click", useController);
  document.querySelectorAll('[data-action="leave"]').forEach((button) =>
    button.addEventListener("click", () => confirmLeave(button)));
  $("leave-controller").addEventListener("click", () => {
    $("settings-dialog").close();
    requestAnimationFrame(() => confirmLeave($("settings-open")));
  });
  $("settings-open").addEventListener("click", () => $("settings-dialog").showModal());
  $("settings-dialog").addEventListener("close", () => $("settings-open").focus());
  $("settings-dialog").addEventListener("change", saveSettings);
  $("leave-dialog").addEventListener("close", () => {
    if (leaveReturnFocus?.isConnected) leaveReturnFocus.focus({preventScroll: true});
    leaveReturnFocus = null;
  });
  $("leave-confirm").addEventListener("click", () => {
    leaveReturnFocus = null;
    $("leave-dialog").close();
    leave();
  });
  $("error-recovery").addEventListener("click", () => {
    render("code", {focus: $("room-code")});
  });
  $("code-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void joinCode($("room-code").value);
  });
  $("room-code").addEventListener("input", () => {
    const digits = $("room-code").value.replace(/\D/g, "").slice(0, 6);
    $("room-code").value = digits.length > 3
      ? `${digits.slice(0, 3)} ${digits.slice(3)}` : digits;
    $("code-error").hidden = true;
    $("room-code").setAttribute("aria-invalid", "false");
  });
  $("copy-link").addEventListener("click", async () => {
    try { await navigator.clipboard.writeText(location.href); announce("Link copied."); }
    catch (_) { announce("Could not copy the link. Use your browser’s Share command."); }
  });
  $("reclaim-controller").addEventListener("click", async () => {
    if (await acquireTabLease(capability, true)) location.reload();
  });
  $("device-name").addEventListener("change", () => {
    const value = $("device-name").value.trim().slice(0, 24);
    $("device-name").value = value;
    try { localStorage.setItem("gb-controller-name", value); } catch (_) {}
  });
  try { $("device-name").value = (localStorage.getItem("gb-controller-name") || "").slice(0, 24); } catch (_) {}

  addEventListener("pagehide", () => {
    leavingPage = true;
    neutralize("pagehide");
  });
  addEventListener("offline", () => reconnect(1));
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible") neutralize("hidden");
    else if (phase === "controller") void requestWakeLock();
  });

  if (testConfig) {
    globalThis.__mdkrControllerTest = Object.freeze({
      approve, reject: showError, reconnect, reconnectComplete, useController,
      showCode: () => render("code", {focus: $("room-code")}), joinCode,
      state: () => ({phase, seat, active, pad: {...pad}, connectionSequence}),
      receiveSignal: (value) => receiveTestSignal?.(value),
    });
  }
  void start();
})();
