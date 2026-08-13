// Dormant launcher-owned MatchRoom signaling adapter. It is intentionally not
// imported by online-room.js while the publisher release gate is disabled.
// The service relays only WebRTC setup and ephemeral P-256 public keys; direct
// DataChannels remain the sole gameplay carrier.
"use strict";

const U32_MAX = 0xffff_ffff;
const SIGNAL_BYTES = 64 * 1024;
const SDP_BYTES = 60 * 1024;
const ICE_BYTES = 4 * 1024;
const META_BYTES = 256;
// One entry per distinct peer endpoint id seen for the lifetime of one socket.
// A legal room contributes at most 3 (the welcome peer list is capped there);
// the headroom covers honest rejoins under a new id.
const MAX_TRACKED_PEER_GENERATIONS = 64;

function u32(value, nonzero = false) {
  return Number.isInteger(value) && value >= (nonzero ? 1 : 0) && value <= U32_MAX;
}

function endpoint(value) {
  if (typeof value !== "string" || !/^[1-9][0-9]{0,19}$/.test(value)) return false;
  try { return BigInt(value) <= 0xffff_ffff_ffff_ffffn; } catch (_) { return false; }
}

function utf8Exceeds(value, limit) {
  let bytes = 0;
  for (let index = 0; index < value.length; index++) {
    const code = value.charCodeAt(index);
    if (code <= 0x7f) bytes++;
    else if (code <= 0x7ff) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff && index + 1 < value.length &&
             value.charCodeAt(index + 1) >= 0xdc00 &&
             value.charCodeAt(index + 1) <= 0xdfff) { bytes += 4; index++; }
    else bytes += 3;
    if (bytes > limit) return true;
  }
  return false;
}

function text(value, limit, empty = false) {
  return typeof value === "string" && (empty || value.length > 0) &&
    !value.includes("\0") && !utf8Exceeds(value, limit);
}

function exact(value, keys) {
  return value && typeof value === "object" && !Array.isArray(value) &&
    Object.keys(value).length === keys.length &&
    Object.keys(value).every(key => keys.includes(key));
}

function publicKey(value) {
  if (typeof value !== "string" || value.length !== 87 ||
      !/^[A-Za-z0-9_-]+$/.test(value)) return false;
  try {
    const raw = atob(value.replace(/-/g, "+").replace(/_/g, "/") +
      "===".slice((value.length + 3) % 4));
    const canonical = btoa(raw).replace(/\+/g, "-").replace(/\//g, "_")
      .replace(/=+$/, "");
    return raw.length === 65 && canonical === value && raw.charCodeAt(0) === 4 &&
      [...raw.slice(1)].some(character => character.charCodeAt(0) !== 0);
  } catch (_) { return false; }
}

function peer(value, self) {
  return exact(value, ["endpointId", "connectionGeneration"]) &&
    endpoint(value.endpointId) && value.endpointId !== self &&
    u32(value.connectionGeneration, true);
}

function parseServerMessage(value, self, generation) {
  if (!value || value.protocolVersion !== 1 || typeof value.type !== "string") return null;
  if (value.type === "signal_welcome" && generation === 0 &&
      exact(value, ["protocolVersion", "type", "endpointId",
        "connectionGeneration", "peers"]) && value.endpointId === self &&
      u32(value.connectionGeneration, true) && Array.isArray(value.peers) &&
      value.peers.length <= 3 && value.peers.every(item => peer(item, self))) {
    const ids = value.peers.map(item => item.endpointId);
    if (new Set(ids).size !== ids.length || ids.some((id, index) => index > 0 &&
        BigInt(id) <= BigInt(ids[index - 1]))) return null;
    return value;
  }
  if (generation === 0) return null;
  if (value.type === "peer_presence" && exact(value,
      ["protocolVersion", "type", "endpointId", "connectionGeneration", "present"]) &&
      peer({endpointId: value.endpointId,
        connectionGeneration: value.connectionGeneration}, self) &&
      typeof value.present === "boolean") return value;
  if (value.type === "signal_error" && exact(value,
      ["protocolVersion", "type", "sequence", "toEndpointId",
        "toConnectionGeneration", "error"]) && u32(value.sequence, true) &&
      endpoint(value.toEndpointId) && value.toEndpointId !== self &&
      u32(value.toConnectionGeneration, true) &&
      value.error === "peer_unavailable") return value;
  const common = ["protocolVersion", "type", "sequence", "toEndpointId",
    "toConnectionGeneration", "fromEndpointId", "fromConnectionGeneration"];
  if (!u32(value.sequence, true) || value.toEndpointId !== self ||
      value.toConnectionGeneration !== generation || !endpoint(value.fromEndpointId) ||
      value.fromEndpointId === self || !u32(value.fromConnectionGeneration, true)) return null;
  if (value.type === "peer_hello" && exact(value, [...common, "publicKey"]) &&
      publicKey(value.publicKey)) return value;
  if ((value.type === "webrtc_offer" || value.type === "webrtc_answer") &&
      exact(value, [...common, "sdp"]) && text(value.sdp, SDP_BYTES)) return value;
  if (value.type === "webrtc_ice" && exact(value,
      [...common, "candidate", "sdpMid", "sdpMLineIndex", "usernameFragment"]) &&
      text(value.candidate, ICE_BYTES) &&
      (value.sdpMid === null || text(value.sdpMid, META_BYTES, true)) &&
      (value.usernameFragment === null || text(value.usernameFragment, META_BYTES, true)) &&
      (value.sdpMLineIndex === null || (Number.isInteger(value.sdpMLineIndex) &&
        value.sdpMLineIndex >= 0 && value.sdpMLineIndex <= 255))) return value;
  if (value.type === "peer_end" && exact(value, [...common, "reason"]) &&
      (value.reason === "restart" || value.reason === "close")) return value;
  return null;
}

function clientMessage(value, sequence, self) {
  if (!value || typeof value !== "object" || Array.isArray(value) ||
      !endpoint(value.toEndpointId) || value.toEndpointId === self ||
      !u32(value.toConnectionGeneration, true)) return null;
  const common = ["type", "toEndpointId", "toConnectionGeneration"];
  let extra = null;
  if (value.type === "peer_hello" && exact(value, [...common, "publicKey"]) &&
      publicKey(value.publicKey)) extra = {publicKey: value.publicKey};
  else if ((value.type === "webrtc_offer" || value.type === "webrtc_answer") &&
      exact(value, [...common, "sdp"]) && text(value.sdp, SDP_BYTES)) {
    extra = {sdp: value.sdp};
  } else if (value.type === "webrtc_ice" && exact(value,
      [...common, "candidate", "sdpMid", "sdpMLineIndex", "usernameFragment"]) &&
      text(value.candidate, ICE_BYTES) &&
      (value.sdpMid === null || text(value.sdpMid, META_BYTES, true)) &&
      (value.usernameFragment === null || text(value.usernameFragment, META_BYTES, true)) &&
      (value.sdpMLineIndex === null || (Number.isInteger(value.sdpMLineIndex) &&
        value.sdpMLineIndex >= 0 && value.sdpMLineIndex <= 255))) {
    extra = {candidate: value.candidate, sdpMid: value.sdpMid,
      sdpMLineIndex: value.sdpMLineIndex, usernameFragment: value.usernameFragment};
  } else if (value.type === "peer_end" && exact(value, [...common, "reason"]) &&
      (value.reason === "restart" || value.reason === "close")) {
    extra = {reason: value.reason};
  }
  return extra ? {protocolVersion: 1, type: value.type, sequence,
    toEndpointId: value.toEndpointId,
    toConnectionGeneration: value.toConnectionGeneration, ...extra} : null;
}

export function createMatchSignalClient(options) {
  if (!options || !/^[A-Za-z0-9_-]{22}$/.test(options.roomId || "") ||
      !endpoint(options.endpointId) || !/^[A-Za-z0-9_-]{43}$/.test(
        options.credential || "")) throw new TypeError("invalid match signal identity");
  const page = new URL(options.pageOrigin || globalThis.location?.href || "https://invalid/");
  const service = new URL(options.serviceOrigin || page.origin, page);
  if (!["http:", "https:"].includes(page.protocol) ||
      !["http:", "https:"].includes(service.protocol) ||
      service.origin !== page.origin) throw new TypeError("cross-origin signaling refused");
  const Socket = options.WebSocketImpl || globalThis.WebSocket;
  if (typeof Socket !== "function") throw new TypeError("WebSocket is unavailable");
  const onEvent = typeof options.onEvent === "function" ? options.onEvent : () => {};
  const onState = typeof options.onState === "function" ? options.onState : () => {};
  const timeoutMs = Math.max(2000, Math.min(30000, Number(options.timeoutMs) || 10000));
  let phase = "idle";
  let socket = null;
  let generation = 0;
  let nextSequence = 1;
  let timer = 0;
  let settle = null;
  let credential = options.credential;
  const peerGenerations = new Map();
  // High-water marks are deliberately never deleted: a departed peer's mark is
  // what rejects a generation rollback if it returns. That makes the map
  // append-only, so it needs an explicit bound. Evicting is not an option —
  // dropping a mark reopens exactly the replay it exists to refuse — so the
  // socket fails closed instead. A room holds at most 4 endpoints, so reaching
  // this bound means the relay is inventing identities, not that a real room
  // churned.
  const peerGenerationHighWater = new Map();
  const rememberPeerGeneration = (endpointId, connectionGeneration) => {
    if (!peerGenerationHighWater.has(endpointId) &&
        peerGenerationHighWater.size >= MAX_TRACKED_PEER_GENERATIONS) return false;
    peerGenerationHighWater.set(endpointId, connectionGeneration);
    return true;
  };
  const receivedSequences = new Map();
  const sentTargets = new Map();

  const snapshot = () => Object.freeze({phase, connectionGeneration: generation,
    nextSequence, connected: phase === "open"});
  const state = () => { try { onState(snapshot()); } catch (_) {} };
  const fail = (code) => {
    if (phase === "failed" || phase === "closed") return;
    phase = "failed";
    clearTimeout(timer);
    const pending = settle;
    settle = null;
    try { socket?.close?.(4003, "invalid_server_message"); } catch (_) {}
    if (pending) pending.reject(Object.assign(new Error(code), {code}));
    state();
  };

  const connect = () => {
    if (phase === "open") return Promise.resolve(snapshot());
    if (phase === "connecting") return settle.promise;
    if (phase !== "idle") return Promise.reject(Object.assign(
      new Error("signal_client_closed"), {code: "signal_client_closed"}));
    phase = "connecting";
    generation = 0;
    nextSequence = 1;
    peerGenerations.clear();
    peerGenerationHighWater.clear();
    receivedSequences.clear();
    sentTargets.clear();
    const url = new URL(`/api/match/${options.roomId}/signal`, service);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    let resolvePromise;
    let rejectPromise;
    const promise = new Promise((resolve, reject) => {
      resolvePromise = resolve; rejectPromise = reject;
    });
    settle = {promise, resolve: resolvePromise, reject: rejectPromise};
    try {
      socket = new Socket(url.toString(),
        ["gb-match-signal-v1", `gb-match.${credential}`]);
    } catch (_) {
      phase = "failed";
      settle = null;
      rejectPromise(Object.assign(new Error("signal_transport_lost"),
        {code: "signal_transport_lost"}));
      state();
      return promise;
    }
    socket.addEventListener("open", () => {
      if (socket?.protocol !== "gb-match-signal-v1") {
        fail("invalid_signal_subprotocol");
      }
    });
    socket.addEventListener("message", event => {
      if (phase !== "connecting" && phase !== "open") return;
      if (typeof event.data !== "string" || utf8Exceeds(event.data, SIGNAL_BYTES)) {
        fail("invalid_signal_message"); return;
      }
      let raw;
      try { raw = JSON.parse(event.data); } catch (_) {
        fail("invalid_signal_message"); return;
      }
      const value = parseServerMessage(raw, options.endpointId, generation);
      if (!value) { fail("invalid_signal_message"); return; }
      if (value.type === "signal_welcome") {
        for (const peerValue of value.peers) {
          peerGenerations.set(peerValue.endpointId, peerValue.connectionGeneration);
          if (!rememberPeerGeneration(peerValue.endpointId,
              peerValue.connectionGeneration)) {
            fail("peer_generation_overflow"); return;
          }
        }
        generation = value.connectionGeneration;
        phase = "open";
        clearTimeout(timer);
        const pending = settle;
        settle = null;
        state();
        try { onEvent(Object.freeze(structuredClone(value))); } catch (_) {}
        pending?.resolve(snapshot());
      } else {
        if (value.type === "peer_presence") {
          const current = peerGenerations.get(value.endpointId);
          const highWater = peerGenerationHighWater.get(value.endpointId) || 0;
          if ((value.present && value.connectionGeneration <= highWater) ||
              (!value.present && current !== value.connectionGeneration)) {
            fail("invalid_signal_message"); return;
          }
          if (value.present) {
            if (!rememberPeerGeneration(value.endpointId,
                value.connectionGeneration)) {
              fail("peer_generation_overflow"); return;
            }
            peerGenerations.set(value.endpointId, value.connectionGeneration);
            receivedSequences.delete(value.endpointId);
          } else {
            peerGenerations.delete(value.endpointId);
            receivedSequences.delete(value.endpointId);
          }
        } else if (value.type === "signal_error") {
          const target = sentTargets.get(value.sequence);
          if (!target || target.endpointId !== value.toEndpointId ||
              target.connectionGeneration !== value.toConnectionGeneration) {
            fail("invalid_signal_message"); return;
          }
          sentTargets.delete(value.sequence);
        } else {
          const prior = receivedSequences.get(value.fromEndpointId) || 0;
          if (peerGenerations.get(value.fromEndpointId) !==
                value.fromConnectionGeneration || value.sequence <= prior) {
            fail("invalid_signal_message"); return;
          }
          receivedSequences.set(value.fromEndpointId, value.sequence);
        }
        try { onEvent(Object.freeze(structuredClone(value))); } catch (_) {}
      }
    });
    socket.addEventListener("close", () => {
      if (phase === "closed" || phase === "failed") return;
      clearTimeout(timer);
      const pending = settle;
      settle = null;
      phase = "failed";
      if (pending) pending.reject(Object.assign(new Error("signal_transport_lost"),
        {code: "signal_transport_lost"}));
      state();
    });
    socket.addEventListener("error", () => {
      if (phase === "connecting") fail("signal_transport_lost");
    });
    timer = setTimeout(() => fail("signal_timeout"), timeoutMs);
    state();
    return promise;
  };

  const send = value => {
    if (phase !== "open" || socket?.readyState !== 1 || !u32(nextSequence, true)) {
      throw Object.assign(new Error("signal_not_connected"), {code: "signal_not_connected"});
    }
    const message = clientMessage(value, nextSequence, options.endpointId);
    if (!message) throw new TypeError("invalid match signal message");
    if (peerGenerations.get(message.toEndpointId) !==
        message.toConnectionGeneration) {
      throw Object.assign(new Error("signal_peer_unavailable"),
        {code: "signal_peer_unavailable"});
    }
    socket.send(JSON.stringify(message));
    sentTargets.set(nextSequence, {endpointId: message.toEndpointId,
      connectionGeneration: message.toConnectionGeneration});
    return nextSequence++;
  };

  const close = () => {
    if (phase === "closed") return;
    clearTimeout(timer);
    const pending = settle;
    settle = null;
    phase = "closed";
    generation = 0;
    peerGenerations.clear();
    peerGenerationHighWater.clear();
    receivedSequences.clear();
    sentTargets.clear();
    credential = "";
    try { socket?.close?.(1000, "launcher_route_changed"); } catch (_) {}
    socket = null;
    if (pending) pending.reject(Object.assign(new Error("signal_client_closed"),
      {code: "signal_client_closed"}));
    state();
  };

  return Object.freeze({connect, send, close, snapshot});
}
