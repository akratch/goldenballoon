"use strict";

(() => {
  const entryFragment = (() => {
    const raw = location.hash.startsWith("#") ? location.hash.slice(1) : "";
    if (!location.hash) return {raw: "", scrubbed: true};
    const exactRoute = location.pathname === "/controller/" && !location.search;
    try { globalThis.history.replaceState(null, "", "/controller/"); }
    catch (_) {
      location.replace("/controller/");
      return {raw: "", scrubbed: false};
    }
    return {raw: exactRoute ? raw : "", scrubbed: true};
  })();
  if (!entryFragment.scrubbed) return;
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
  let errorRecoveryAction = "code";
  const maxControllerResponseBytes = 16 * 1024;
  const controllerRequestTimeoutMs = 10_000;
  const pageBoundRequests = new Set();

  const errors = {
    missing_link: ["Controller link missing",
      "Scan the QR on the display again or enter its current room code."],
    embedded: ["Continue in Safari or Chrome",
      "Share this controller link to Safari or Chrome. If it has no invitation, enter the current room code there."],
    unsupported: ["Open in an updated browser",
      "This browser cannot make the direct controller connection. Share this controller link to an updated Safari or Chrome."],
    update_required: ["Controller update required",
      "The display uses a newer controller protocol. Refresh this controller."],
    protocol_update_required: ["This controller page is out of date",
      "Reload to update."],
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
      "This tab released its controls because another tab took over. Enter the current room code if you want to join again."],
    insecure: ["Secure connection required",
      "Phone controllers require HTTPS. Continue to the secure version of this controller page before pairing."],
    seat_reclaimed: ["Phone controller removed",
      "The display removed this phone controller. Its controls are neutral. Enter the newest code to join again."],
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

  function showError(id, recoveryLabel = "Enter another code",
                     recoveryAction = "code") {
    neutralize("error");
    const copy = errors[id] || ["Couldn’t join", "Try the current QR or room code again."];
    $("error-title").textContent = copy[0];
    $("error-message").textContent = copy[1];
    $("error-recovery").textContent = recoveryLabel;
    errorRecoveryAction = recoveryAction;
    $("copy-link").textContent = capability ? "Copy private link" : "Copy controller page";
    $("copy-link").hidden = !["embedded", "unsupported"].includes(id);
    render("error", {focus: $("error-recovery")});
  }

  function bytesFromCapability(value) {
    if (!/^[A-Za-z0-9_-]{43}$/.test(value)) return null;
    try {
      const base64 = value.replace(/-/g, "+").replace(/_/g, "/") +
        "===".slice((value.length + 3) % 4);
      const binary = atob(base64);
      if (binary.length !== 32) return null;
      return Uint8Array.from(binary, (character) => character.charCodeAt(0));
    } catch (_) { return null; }
  }

  function exactKeys(value, expected) {
    if (!value || typeof value !== "object" || Array.isArray(value)) return false;
    const keys = Object.keys(value).sort();
    const wanted = [...expected].sort();
    return keys.length === wanted.length &&
      keys.every((key, index) => key === wanted[index]);
  }

  function normalizedDeviceName(value) {
    const safe = String(value || "").normalize("NFC")
      .replace(/[\u0000-\u001f\u007f-\u009f\u200b-\u200f\u2028\u2029\u202a-\u202e\u2060\u2066-\u2069\ufeff]/g,
        "")
      .trim();
    return [...safe].slice(0, 24).join("");
  }

  async function readControllerJson(response) {
    const declared = response.headers?.get?.("content-length");
    if (declared && /^\d+$/.test(declared) &&
        Number(declared) > maxControllerResponseBytes) {
      try { await response.body?.cancel?.(); } catch (_) {}
      throw new Error("service_unavailable");
    }
    if (!response.body?.getReader) {
      const text = await response.text();
      if (new TextEncoder().encode(text).byteLength > maxControllerResponseBytes) {
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
            total + value.byteLength > maxControllerResponseBytes) {
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

  function normalizedControllerInfo(value) {
    const keys = ["controllerId", "credential", "roomId", "protocol",
      "hostPublicKey"];
    if (!exactKeys(value, keys) ||
        !/^[A-Za-z0-9_-]{22}$/.test(value.controllerId) ||
        !/^[A-Za-z0-9_-]{43}$/.test(value.credential) ||
        !/^[A-Za-z0-9_-]{22}$/.test(value.roomId) || value.protocol !== 1 ||
        !/^[A-Za-z0-9_-]{87}$/.test(value.hostPublicKey)) return null;
    return Object.freeze({...value});
  }

  function consumeFragment() {
    const raw = entryFragment.raw;
    entryFragment.raw = "";
    if (!entryFragment.scrubbed) return "";
    if (testConfig) return testConfig.codeMode
      ? "" : (testConfig.capability || "test-controller-capability");
    return bytesFromCapability(raw) ? raw : "";
  }

  function controllerInviteUrl() {
    if (!capability) return "";
    const url = new URL("/controller/", location.origin);
    url.hash = capability;
    return url.toString();
  }

  function controllerRecoveryUrl() {
    return secureControllerRecoveryUrl() || controllerInviteUrl() ||
      new URL("/controller/", location.origin).toString();
  }

  function loopbackHostname(hostname) {
    const value = String(hostname || "").toLowerCase();
    return value === "localhost" || value.endsWith(".localhost") ||
      value === "127.0.0.1" || value === "[::1]";
  }

  function trustedControllerLocation() {
    try {
      const url = new URL(location.href);
      return isSecureContext === true && !url.username && !url.password &&
        (url.protocol === "https:" ||
          (url.protocol === "http:" && loopbackHostname(url.hostname)));
    } catch (_) { return false; }
  }

  function secureControllerRecoveryUrl() {
    try {
      const url = new URL("/controller/", location.href);
      if (url.protocol !== "http:" || loopbackHostname(url.hostname)) return "";
      url.protocol = "https:";
      url.port = "";
      if (capability) url.hash = capability;
      return url.toString();
    } catch (_) { return ""; }
  }

  function shareRecoveryLabel() {
    return capability ? "Share private link" : "Share controller page";
  }

  async function copyControllerInvite() {
    const privateLink = !!capability;
    const inviteUrl = controllerRecoveryUrl();
    try {
      await navigator.clipboard.writeText(inviteUrl);
      announce(privateLink ?
        "Private link copied. Open Safari or Chrome and paste it there." :
        "Controller page copied. Open it in Safari or Chrome, then enter the room code.");
      return true;
    } catch (_) {
      announce(privateLink ?
        "Could not copy the private link. Scan the current QR or enter its room code." :
        "Could not copy the controller page. Open it manually, then enter the room code.");
      return false;
    }
  }

  async function shareControllerInvite() {
    const privateLink = !!capability;
    const inviteUrl = controllerRecoveryUrl();
    if (!navigator.share) return copyControllerInvite();
    try {
      await navigator.share({title: "Golden Balloon phone controller",
        text: privateLink
          ? "Open this private controller link in Safari or Chrome."
          : "Open this controller page in Safari or Chrome, then enter the current room code.",
        url: inviteUrl});
      announce(privateLink ? "Private controller link shared." :
        "Controller page shared. Enter the current room code there.");
      return true;
    } catch (error) {
      if (error?.name === "AbortError") {
        announce(privateLink ? "Sharing cancelled. Your invitation is still private." :
          "Sharing cancelled. The controller page was not sent.");
        return false;
      }
      return copyControllerInvite();
    }
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

  function tabLeaseId() {
    const bytes = crypto.getRandomValues(new Uint8Array(12));
    return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  function waitForTabLease(milliseconds) {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
  }

  function handleTabLeaseLoss() {
    if (leavingPage || phase === "error") return;
    capability = "";
    showError("duplicate_controller");
    try { transport?.close?.(); } catch (_) {}
    if (heartbeat !== null) { clearInterval(heartbeat); heartbeat = null; }
    if (wakeLock) wakeLock.release().catch(() => {});
    announce("This tab released the controller because another tab took over.");
  }

  function tabLeaseChannel(name, listener) {
    if (typeof BroadcastChannel !== "function") return null;
    try {
      const channel = new BroadcastChannel(name);
      channel.addEventListener("message", listener);
      return channel;
    } catch (_) { return null; }
  }

  function postTabLease(channel, value) {
    try { channel?.postMessage(value); } catch (_) {}
  }

  async function acquireWebTabLease(name, force) {
    const id = tabLeaseId();
    let owned = false;
    let releaseHold = null;
    let channel = null;
    const closeChannel = () => {
      try { channel?.close(); } catch (_) {}
      channel = null;
    };
    const relinquish = (taken = false) => {
      if (!owned && !releaseHold) { closeChannel(); return; }
      owned = false;
      const release = releaseHold;
      releaseHold = null;
      if (releaseTabLease === relinquish) releaseTabLease = null;
      closeChannel();
      if (release) release();
      if (taken) handleTabLeaseLoss();
    };
    channel = tabLeaseChannel(name, (event) => {
      const message = event.data;
      if (!message || typeof message !== "object" || message.id === id) return;
      if (message.type === "probe" && owned) {
        postTabLease(channel, {type: "held", id});
      } else if (message.type === "steal" && owned) {
        postTabLease(channel, {type: "released", id, to: message.id});
        relinquish(true);
      }
    });
    if (force) {
      postTabLease(channel, {type: "steal", id});
      await waitForTabLease(120);
    }
    return new Promise((resolve) => {
      let settled = false;
      const settle = (value) => {
        if (settled) return;
        settled = true;
        resolve(value);
      };
      navigator.locks.request(name, {mode: "exclusive", ifAvailable: true},
        async (lock) => {
          if (!lock) { closeChannel(); settle(false); return; }
          owned = true;
          settle(true);
          await new Promise((release) => {
            releaseHold = release;
            releaseTabLease = relinquish;
          });
        }).catch(() => {
          closeChannel();
          settle(false);
        });
    });
  }

  function parsedFallbackLease(raw) {
    try {
      const value = JSON.parse(raw || "null");
      if (!value || typeof value.id !== "string" || value.id.length > 80 ||
          typeof value.name !== "string" ||
          !/^golden-balloon-controller-[0-9a-f]{24}$/.test(value.name) ||
          !Number.isSafeInteger(value.expiresAt) || value.expiresAt < 0) return null;
      return value;
    } catch (_) { return null; }
  }

  async function acquireFallbackTabLease(name, force) {
    const id = tabLeaseId();
    const storageKey = "gb-controller-tab-lease";
    const ttlMs = 15_000;
    let storageAvailable = true;
    let blocked = false;
    let takeoverAcknowledged = false;
    let owned = false;
    let beatTimer = null;
    let channel = null;
    const read = () => {
      try { return parsedFallbackLease(localStorage.getItem(storageKey)); }
      catch (_) { storageAvailable = false; return null; }
    };
    const write = () => {
      try {
        localStorage.setItem(storageKey, JSON.stringify({
          name, id, expiresAt: Date.now() + ttlMs,
        }));
        return true;
      } catch (_) { storageAvailable = false; return false; }
    };
    const closeChannel = () => {
      try { channel?.close(); } catch (_) {}
      channel = null;
    };
    const removeOwnStorage = () => {
      if (!storageAvailable) return;
      try {
        if (read()?.id === id) localStorage.removeItem(storageKey);
      } catch (_) {}
    };
    const release = (taken = false) => {
      if (!owned && beatTimer === null) { closeChannel(); return; }
      owned = false;
      if (beatTimer !== null) clearInterval(beatTimer);
      beatTimer = null;
      removeEventListener("storage", storageListener);
      removeOwnStorage();
      closeChannel();
      if (releaseTabLease === release) releaseTabLease = null;
      if (taken) handleTabLeaseLoss();
    };
    const storageListener = (event) => {
      if (!owned || event.key !== storageKey) return;
      const next = parsedFallbackLease(event.newValue);
      if (!next || next.id !== id) release(true);
    };
    channel = tabLeaseChannel(name, (event) => {
      const message = event.data;
      if (!message || typeof message !== "object" || message.id === id) return;
      if ((message.type === "probe" || message.type === "candidate") && owned) {
        postTabLease(channel, {type: "held", id});
      } else if (message.type === "steal" && owned) {
        postTabLease(channel, {type: "released", id, to: message.id});
        release(true);
      } else if (message.type === "released" && message.to === id) {
        takeoverAcknowledged = true;
      } else if (!owned && (message.type === "held" ||
          (message.type === "candidate" && String(message.id) < id))) {
        blocked = true;
      }
    });
    const initialLease = read();
    if (!storageAvailable && !channel) return false;
    if (force) {
      const liveStoredOwner = initialLease && initialLease.id !== id &&
        initialLease.expiresAt > Date.now();
      if (liveStoredOwner && !channel) return false;
      postTabLease(channel, {type: "steal", id});
      await waitForTabLease(120);
      if (liveStoredOwner && !takeoverAcknowledged) {
        closeChannel();
        return false;
      }
    }
    postTabLease(channel, {type: "probe", id});
    postTabLease(channel, {type: "candidate", id});
    await waitForTabLease(120);
    if (blocked) { closeChannel(); return false; }
    const prior = read();
    if (!force && prior && prior.id !== id && prior.expiresAt > Date.now()) {
      closeChannel();
      return false;
    }
    if (storageAvailable && !write() && !channel) {
      closeChannel();
      return false;
    }
    await waitForTabLease(40);
    if (blocked || (storageAvailable && read()?.id !== id)) {
      removeOwnStorage();
      closeChannel();
      return false;
    }
    owned = true;
    addEventListener("storage", storageListener);
    beatTimer = setInterval(() => {
      const current = read();
      if (storageAvailable && current && current.id !== id &&
          current.expiresAt > Date.now()) {
        release(true);
        return;
      }
      if (storageAvailable) write();
      postTabLease(channel, {type: "held", id});
    }, 4_000);
    releaseTabLease = release;
    postTabLease(channel, {type: "held", id});
    return true;
  }

  async function acquireTabLease(value, force = false) {
    const name = "golden-balloon-controller-" + await tabKey(value);
    if (!testConfig?.forceFallbackLease && navigator.locks &&
        typeof navigator.locks.request === "function") {
      if (testState) testState.leaseMode = "web-lock";
      return acquireWebTabLease(name, force);
    }
    if (testState) testState.leaseMode = "broadcast-storage";
    return acquireFallbackTabLease(name, force);
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
    let reconnectExhausted = false;
    let controlStableTimer = null;
    let signalingLimited = false;
    let publishedControllerTransition = 0;
    let publishedControllerFingerprint = "";
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
      if (testConfig?.request) {
        let timer = 0;
        const timeout = new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error("service_unavailable")),
            controllerRequestTimeoutMs);
        });
        try {
          const value = await Promise.race([
            Promise.resolve(testConfig.request(path, body)), timeout]);
          return value;
        } finally { clearTimeout(timer); }
      }
      const controller = new AbortController();
      pageBoundRequests.add(controller);
      const timer = setTimeout(() => controller.abort(), controllerRequestTimeoutMs);
      try {
        const response = await fetch(path, {
          method: "POST", headers: {"content-type": "application/json"},
          cache: "no-store", credentials: "omit", referrerPolicy: "no-referrer",
          signal: controller.signal, body: JSON.stringify(body),
        });
        if (testState) testState.requests.push(response.url);
        const value = await readControllerJson(response).catch(() => ({}));
        if (!response.ok) throw new Error(
          typeof value.error === "string" ? value.error : "service_unavailable");
        const normalized = normalizedControllerInfo(value);
        if (!normalized) throw new Error("service_unavailable");
        return normalized;
      } catch (error) {
        if (error?.name === "AbortError") throw new Error("service_unavailable");
        throw error;
      } finally {
        clearTimeout(timer);
        pageBoundRequests.delete(controller);
      }
    }

    function signal(value) {
      if (!controllerInfo) return false;
      if (testConfig?.directSignaling) {
        testState.signals ||= [];
        const encodedValue = {...value, controllerId: controllerInfo.controllerId};
        testState.signals.push(JSON.parse(JSON.stringify(encodedValue)));
        if (typeof testConfig.sendSignal === "function") testConfig.sendSignal(encodedValue);
        return true;
      }
      if (controlSocket?.readyState !== WebSocket.OPEN) return false;
      const encoded = JSON.stringify({...value, controllerId: controllerInfo.controllerId});
      if (encoded.length > 60 * 1024 ||
          new TextEncoder().encode(encoded).byteLength > 60 * 1024) return false;
      try {
        controlSocket.send(encoded);
        return true;
      } catch (_) {
        failControlSocket(controlSocket, controlGeneration,
          4011, "controller_signal_send_failed");
        return false;
      }
    }

    async function acceptOffer(message) {
      if (!controllerInfo || message.to !== controllerInfo.controllerId || !message.sdp ||
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
        try {
          connection = new RTCPeerConnection({iceServers: [{
            urls: "stun:stun.cloudflare.com:3478",
          }]});
        } catch (_) {
          reconnect(1);
          announce("Could not open a direct controller connection. Trying again.");
          return;
        }
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
            try {
              controlChannel.send(JSON.stringify({
                type: "controller_ready", protocol: 1,
                controllerId: controllerInfo.controllerId,
                connectionSequence,
                capabilities: {vibration: typeof navigator.vibrate === "function"},
              }));
            } catch (_) {
              directTransportLost(connection, true);
              return;
            }
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
              if (value.type === "ping" && value.protocol === 1 &&
                  Number.isInteger(value.nonce) && value.nonce >= 0 &&
                  value.nonce <= 0xffffffff) {
                try {
                  controlChannel.send(JSON.stringify({type: "pong", protocol: 1,
                    nonce: value.nonce}));
                } catch (_) { directTransportLost(connection, true); }
              } else if (value.type === "input_test_ack" && value.nonce === inputTestNonce) {
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

    function normalizedControllerState(value) {
      const keys = ["type", "transitionId", "phase", "seat",
        "leaseGeneration", "connectionSequence"];
      if (!exactKeys(value, keys) || value.type !== "controller_state" ||
          !Number.isSafeInteger(value.transitionId) || value.transitionId < 1 ||
          value.transitionId > 4097 ||
          !["pending", "approved", "leased", "connected", "closed"]
            .includes(value.phase) ||
          !Number.isInteger(value.leaseGeneration) ||
          value.leaseGeneration < 0 || value.leaseGeneration > 0xffff_ffff ||
          !Number.isInteger(value.connectionSequence) ||
          value.connectionSequence < 0 ||
          value.connectionSequence > 0xffff_ffff) return null;
      if (value.seat !== null && (!Number.isInteger(value.seat) ||
          value.seat < 1 || value.seat > 4)) return null;
      if (["approved", "leased", "connected"].includes(value.phase) &&
          (value.seat === null || value.leaseGeneration < 1 ||
           value.connectionSequence < 1)) return null;
      return Object.freeze({...value});
    }

    function acceptControllerState(value, handleMessage) {
      const normalized = normalizedControllerState(value);
      if (!normalized) throw new Error("invalid_controller_state");
      if (normalized.transitionId < publishedControllerTransition) return false;
      const fingerprint = JSON.stringify(normalized);
      if (normalized.transitionId === publishedControllerTransition) {
        if (fingerprint !== publishedControllerFingerprint) {
          throw new Error("equivocated_controller_state");
        }
        return false;
      }
      handleMessage(normalized);
      publishedControllerTransition = normalized.transitionId;
      publishedControllerFingerprint = fingerprint;
      return true;
    }

    function clearControlTimers() {
      if (reconnectTimer !== null) clearTimeout(reconnectTimer);
      if (controlStableTimer !== null) clearTimeout(controlStableTimer);
      reconnectTimer = null;
      controlStableTimer = null;
    }

    function showSignalingInterruption(attempt) {
      if (directChannelsOpen() && ["assigned", "controller"].includes(phase)) {
        if (!signalingLimited) {
          signalingLimited = true;
          setConnection("limited",
            "Controller connected directly; room service reconnecting");
          announce("Controller remains connected directly. Room service reconnecting.");
        }
        return;
      }
      signalingLimited = false;
      reconnect(attempt);
    }

    function scheduleControlReconnect(value, generation) {
      if (!controllerInfo || leavingPage || generation !== controlGeneration ||
          reconnectTimer !== null) return;
      if (reconnectAttempt >= 5) {
        reconnectExhausted = true;
        if (directChannelsOpen() && ["assigned", "controller"].includes(phase)) {
          signalingLimited = true;
          setConnection("limited",
            "Controller connected directly; room service retry paused");
          announce("Direct controls still work. Room service retry is paused; open settings only if you need to leave.");
        } else {
          reconnect(5);
          $("reconnect-progress").textContent =
            "Automatic reconnect paused after five attempts. Try again when the connection is ready.";
          requestAnimationFrame(() => $("controller-retry").focus());
        }
        return;
      }
      const attempt = ++reconnectAttempt;
      showSignalingInterruption(attempt);
      const delay = Math.min(4800, 300 * (2 ** Math.min(attempt - 1, 4)));
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        if (generation === controlGeneration) connectControl(value);
      }, delay);
    }

    function failControlSocket(connection, generation, code, reason) {
      if (connection !== controlSocket || generation !== controlGeneration) return;
      controlSocket = null;
      if (controlStableTimer !== null) clearTimeout(controlStableTimer);
      controlStableTimer = null;
      try { connection.close(code, reason); } catch (_) {}
      scheduleControlReconnect(controllerInfo, generation);
    }

    function failControlSocketSetup(generation) {
      if (generation !== controlGeneration) return;
      controlSocket = null;
      if (controlStableTimer !== null) clearTimeout(controlStableTimer);
      controlStableTimer = null;
      scheduleControlReconnect(controllerInfo, generation);
    }

    function connectControl(value) {
      if (!value?.roomId || !value?.credential || leavingPage) return false;
      if (!controllerInfo || controllerInfo.roomId !== value.roomId ||
          controllerInfo.controllerId !== value.controllerId) {
        publishedControllerTransition = 0;
        publishedControllerFingerprint = "";
      }
      controllerInfo = value;
      const handleMessage = (update) => {
        if (update.type === "controller_state" && update.phase === "pending") {
          signalingLimited = false;
          if (phase === "reconnecting" && reconnectResume === "waiting") {
            render("waiting", {focus: $("device-name")});
          }
        } else if (update.type === "controller_state" &&
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
          showError(phase === "waiting" ? "approval_rejected" : "seat_reclaimed");
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
      clearControlTimers();
      const previous = controlSocket;
      controlSocket = null;
      const generation = ++controlGeneration;
      try { previous?.close?.(1000, "controller_socket_replaced"); } catch (_) {}
      let connection;
      try {
        const url = new URL(`/api/party/${value.roomId}/connect`, location.origin);
        url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
        connection = new WebSocket(url, ["gb-control-v1",
          `gb-controller.${value.credential}`]);
        controlSocket = connection;
        connection.binaryType = "arraybuffer";
      } catch (_) {
        failControlSocketSetup(generation);
        return false;
      }
      try {
        connection.addEventListener("open", () => {
          if (connection !== controlSocket || generation !== controlGeneration) return;
          reconnectExhausted = false;
          controlStableTimer = setTimeout(() => {
            controlStableTimer = null;
            if (connection === controlSocket && generation === controlGeneration) {
              reconnectAttempt = 0;
            }
          }, 30_000);
          signal({type: "controller_hello"});
        });
        connection.addEventListener("message", (event) => {
          if (connection !== controlSocket || generation !== controlGeneration) return;
          if (typeof event.data !== "string" || event.data.length > 64 * 1024 ||
              new TextEncoder().encode(event.data).byteLength > 64 * 1024) {
            failControlSocket(connection, generation, 4009,
              "invalid_controller_message");
            return;
          }
          try {
            const update = JSON.parse(event.data);
            if (update?.type === "controller_state") {
              acceptControllerState(update, handleMessage);
            } else {
              handleMessage(update);
            }
          } catch (_) {
            failControlSocket(connection, generation, 4003,
              "invalid_controller_message");
          }
        });
        connection.addEventListener("error", () => {
          failControlSocket(connection, generation, 4011,
            "controller_socket_error");
        });
        connection.addEventListener("close", (event) => {
          if (connection !== controlSocket || generation !== controlGeneration ||
              leavingPage) return;
          controlSocket = null;
          if (controlStableTimer !== null) clearTimeout(controlStableTimer);
          controlStableTimer = null;
          if (event.code === 4000 &&
              ["approval_rejected", "seat_reclaimed", "host_closed", "room_expired",
               "duplicate_controller"].includes(event.reason)) {
            controlGeneration++;
            clearControlTimers();
            showError(event.reason);
            if (heartbeat !== null) { clearInterval(heartbeat); heartbeat = null; }
            if (wakeLock) wakeLock.release().catch(() => {});
            if (peer) { peer.close(); peer = null; }
            stateChannel = null;
            controlChannel = null;
            controllerInfo = null;
            publishedControllerTransition = 0;
            publishedControllerFingerprint = "";
            return;
          }
          scheduleControlReconnect(value, generation);
        });
      } catch (_) {
        failControlSocket(connection, generation, 4011,
          "controller_socket_setup_failed");
        return false;
      }
      return true;
    }

    const api = {
      sendState(bytes) {
        if (stateChannel?.readyState !== "open" || stateChannel.bufferedAmount > 4096) return;
        try { stateChannel.send(bytes); }
        catch (_) { if (peer) directTransportLost(peer, true); }
      },
      neutral() {},
      inputTest(pressed) {
        if (!pressed || controlChannel?.readyState !== "open") return true;
        inputTestNonce = (inputTestNonce + 1) >>> 0;
        try {
          controlChannel.send(JSON.stringify({type: "input_test", nonce: inputTestNonce}));
        } catch (_) {
          if (peer) directTransportLost(peer, true);
        }
        return true;
      },
      close() {
        signalingLimited = false;
        controlGeneration++;
        clearControlTimers();
        reconnectAttempt = 0;
        reconnectExhausted = false;
        publishedControllerTransition = 0;
        publishedControllerFingerprint = "";
        if (controlSocket) {
          try { controlSocket.close(1000, "controller_left"); } catch (_) {}
          controlSocket = null;
        }
        if (peer) { try { peer.close(); } catch (_) {} peer = null; }
        stateChannel = null;
        controlChannel = null;
        controllerInfo = null;
      },
      retry() {
        if (!controllerInfo || leavingPage) return false;
        reconnectAttempt = 0;
        reconnectExhausted = false;
        return connectControl(controllerInfo);
      },
      async redeem(secret) {
        const identity = await identityPromise;
        const value = await post("/api/controller/redeem", {capability: secret,
          protocol: 1, name: normalizedDeviceName($("device-name").value),
          controllerPublicKey: identity.publicKey});
        const phrase = value.hostPublicKey
          ? await globalThis.MDKRPartySas.phrase(
          identity.privateKey, value.hostPublicKey, {roomId: value.roomId,
            hostPublicKey: value.hostPublicKey,
            controllerPublicKey: identity.publicKey})
          : value.phrase;
        const connected = {...value, phrase};
        connectControl(connected);
        return connected;
      },
      async redeemCode(code) {
        const identity = await identityPromise;
        const value = await post("/api/controller/code", {code, protocol: 1,
          name: normalizedDeviceName($("device-name").value),
          controllerPublicKey: identity.publicKey});
        const phrase = value.hostPublicKey
          ? await globalThis.MDKRPartySas.phrase(
          identity.privateKey, value.hostPublicKey, {roomId: value.roomId,
            hostPublicKey: value.hostPublicKey,
            controllerPublicKey: identity.publicKey})
          : value.phrase;
        const connected = {...value, phrase};
        connectControl(connected);
        return connected;
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
      if (leavingPage) return;
      if (result.protocol !== 1) {
        showError("update_required", "Refresh controller", "reload");
        return;
      }
      $("pairing-phrase").textContent = result.phrase || "Bright Balloon";
      render("waiting", {focus: $("device-name")});
      if (testConfig && testConfig.autoApprove !== false) {
        setTimeout(() => approve(testConfig.seat || 1), testConfig.approveDelayMs || 0);
      }
    } catch (error) {
      if (leavingPage) return;
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
    $("controller-retry").disabled = false;
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

  function resumeControllerInput() {
    if (phase !== "controller") return;
    if (!active) {
      active = true;
      padHistory = [];
      publishPad(true);
    }
    void requestWakeLock();
  }

  function leave() {
    capability = "";
    neutralize("leave");
    try { transport?.close?.(); } catch (_) {}
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
    if (testConfig?.entryMode === "embedded") {
      showError("embedded", shareRecoveryLabel(), "share");
      return;
    }
    if (testConfig?.entryMode === "duplicate") {
      render("duplicate", {focus: $("reclaim-controller")});
      return;
    }
    if (!testConfig) {
      try {
        if (window.top !== window.self) {
          showError("embedded", shareRecoveryLabel(), "share"); return;
        }
      } catch (_) {
        showError("embedded", shareRecoveryLabel(), "share"); return;
      }
      if (!trustedControllerLocation()) {
        const secureUrl = secureControllerRecoveryUrl();
        showError("insecure", secureUrl ? "Open secure controller page" :
          "Enter another code", secureUrl ? "secure" : "code");
        return;
      }
      if (!("PointerEvent" in window) || !("RTCPeerConnection" in window) ||
          !globalThis.crypto?.subtle || !globalThis.MDKRPartySas) {
        showError("unsupported", shareRecoveryLabel(), "share"); return;
      }
    }
    if (!capability) {
      transport = testConfig ? mockTransport() : networkTransport();
      render("code", {focus: $("room-code")});
      return;
    }
    let ownsTab;
    try { ownsTab = await acquireTabLease(capability); }
    catch (_) {
      showError("unsupported", shareRecoveryLabel(), "share"); return;
    }
    if (!ownsTab) {
      render("duplicate", {focus: $("reclaim-controller")}); return;
    }
    if (leavingPage) {
      if (releaseTabLease) { releaseTabLease(); releaseTabLease = null; }
      capability = "";
      return;
    }
    transport = testConfig && !testConfig.directSignaling
      ? mockTransport() : networkTransport();
    try {
      const result = await transport.redeem(capability);
      capability = "";
      if (leavingPage) return;
      if (result.protocol !== 1) {
        showError("update_required", "Refresh controller", "reload");
        return;
      }
      $("pairing-phrase").textContent = result.phrase || "Bright Balloon";
      render("waiting", {focus: $("device-name")});
      if (testConfig && testConfig.autoApprove !== false) {
        setTimeout(() => approve(testConfig.seat || 1), testConfig.approveDelayMs || 0);
      }
    } catch (error) {
      capability = "";
      if (leavingPage) return;
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
  $("controller-retry").addEventListener("click", () => {
    $("controller-retry").disabled = true;
    if (transport?.retry?.() === true) {
      $("reconnect-progress").textContent = "Trying the controller room now…";
      announce("Trying the controller room now.");
    } else {
      $("controller-retry").disabled = false;
    }
  });
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
    if (errorRecoveryAction === "share") {
      void shareControllerInvite();
    } else if (errorRecoveryAction === "reload") {
      location.reload();
    } else if (errorRecoveryAction === "secure") {
      const secureUrl = secureControllerRecoveryUrl();
      if (secureUrl) location.replace(secureUrl);
      else render("code", {focus: $("room-code")});
    } else {
      render("code", {focus: $("room-code")});
    }
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
  $("copy-link").addEventListener("click", () => { void copyControllerInvite(); });
  $("reclaim-controller").addEventListener("click", async () => {
    const inviteUrl = controllerInviteUrl();
    try {
      if (inviteUrl && await acquireTabLease(capability, true)) {
        location.replace(inviteUrl);
        // A fragment-only replace stays in this document; force the entry.
        if (location.href === inviteUrl) location.reload();
        return;
      }
    } catch (_) {}
    announce("Could not move the controller. Close the other tab, then try again.");
  });
  $("device-name").addEventListener("change", () => {
    const value = normalizedDeviceName($("device-name").value);
    $("device-name").value = value;
    try { localStorage.setItem("gb-controller-name", value); } catch (_) {}
  });
  try {
    $("device-name").value = normalizedDeviceName(
      localStorage.getItem("gb-controller-name") || "");
  } catch (_) {}

  addEventListener("pagehide", () => {
    leavingPage = true;
    capability = "";
    for (const controller of pageBoundRequests) controller.abort();
    pageBoundRequests.clear();
    neutralize("pagehide");
    try { transport?.close?.(); } catch (_) {}
    if (releaseTabLease) { releaseTabLease(); releaseTabLease = null; }
  });
  addEventListener("pageshow", (event) => {
    if (event.persisted) location.reload();
  });
  addEventListener("freeze", () => neutralize("freeze"));
  addEventListener("resume", resumeControllerInput);
  addEventListener("offline", () => {
    if (["assigned", "controller", "reconnecting"].includes(phase)) reconnect(1);
  });
  addEventListener("online", () => {
    if (phase === "reconnecting") transport?.retry?.();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible") neutralize("hidden");
    else resumeControllerInput();
  });

  if (testConfig) {
    globalThis.__mdkrControllerTest = Object.freeze({
      approve, reject: showError, reconnect, reconnectComplete, useController,
      showCode: () => render("code", {focus: $("room-code")}), joinCode,
      showEmbedded: () => showError("embedded", shareRecoveryLabel(), "share"),
      showDuplicate: () => render("duplicate", {focus: $("reclaim-controller")}),
      inviteUrl: controllerInviteUrl,
      state: () => ({phase, seat, active, pad: {...pad}, connectionSequence}),
      receiveSignal: (value) => receiveTestSignal?.(value),
    });
  }
  void start();
})();
