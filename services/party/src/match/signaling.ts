import {base64Url, fromBase64Url, utf8Exceeds} from "../security";
import {MATCH_LIMITS, parseU64} from "./protocol";

export type MatchSignalClientMessage =
  | {protocolVersion: 1; type: "peer_hello"; sequence: number;
      toEndpointId: string; toConnectionGeneration: number; publicKey: string}
  | {protocolVersion: 1; type: "webrtc_offer" | "webrtc_answer";
      sequence: number; toEndpointId: string; toConnectionGeneration: number;
      sdp: string}
  | {protocolVersion: 1; type: "webrtc_ice"; sequence: number;
      toEndpointId: string; toConnectionGeneration: number; candidate: string;
      sdpMid: string | null; sdpMLineIndex: number | null;
      usernameFragment: string | null}
  | {protocolVersion: 1; type: "peer_end"; sequence: number;
      toEndpointId: string; toConnectionGeneration: number;
      reason: "restart" | "close"};

export interface MatchSignalRateState {
  messages: number;
  windowStartedAt: number;
  lifetimeMessages: number;
}

const BASE_KEYS = ["protocolVersion", "type", "sequence", "toEndpointId",
  "toConnectionGeneration"] as const;

function exactKeys(value: Record<string, unknown>, additional: readonly string[]): boolean {
  const allowed = new Set<string>([...BASE_KEYS, ...additional]);
  const keys = Object.keys(value);
  return keys.length === allowed.size && keys.every(key => allowed.has(key));
}

function generation(value: unknown): value is number {
  return Number.isInteger(value) && Number(value) >= 1 && Number(value) <= 0xffff_ffff;
}

function sequence(value: unknown): value is number {
  return Number.isInteger(value) && Number(value) >= 1 && Number(value) <= 0xffff_ffff;
}

function boundedText(value: unknown, maximum: number, allowEmpty = false): value is string {
  return typeof value === "string" && (allowEmpty || value.length > 0) &&
    !value.includes("\0") && !utf8Exceeds(value, maximum);
}

function validPublicKey(value: unknown): value is string {
  if (typeof value !== "string" || value.length !== 87 ||
      !/^[A-Za-z0-9_-]+$/.test(value)) return false;
  const decoded = fromBase64Url(value);
  return decoded?.byteLength === 65 && base64Url(decoded) === value && decoded[0] === 4 &&
    decoded.subarray(1).some(byte => byte !== 0);
}

/** Parse one exact client-to-service signaling envelope. The authenticated
 * sender is intentionally absent and is injected by MatchRoom. */
export function parseMatchSignalMessage(value: unknown,
                                        senderEndpointId: string): MatchSignalClientMessage | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const item = value as Record<string, unknown>;
  if (item.protocolVersion !== 1 || typeof item.type !== "string" ||
      !sequence(item.sequence) || parseU64(item.toEndpointId) === null ||
      item.toEndpointId === senderEndpointId ||
      !generation(item.toConnectionGeneration)) return null;
  const base = {protocolVersion: 1 as const, type: item.type,
    sequence: Number(item.sequence), toEndpointId: String(item.toEndpointId),
    toConnectionGeneration: Number(item.toConnectionGeneration)};
  if (item.type === "peer_hello" && exactKeys(item, ["publicKey"]) &&
      validPublicKey(item.publicKey)) {
    return {...base, type: "peer_hello", publicKey: item.publicKey};
  }
  if ((item.type === "webrtc_offer" || item.type === "webrtc_answer") &&
      exactKeys(item, ["sdp"]) && boundedText(item.sdp, MATCH_LIMITS.maxSignalSdpBytes)) {
    return {...base, type: item.type, sdp: item.sdp};
  }
  if (item.type === "webrtc_ice" && exactKeys(item,
      ["candidate", "sdpMid", "sdpMLineIndex", "usernameFragment"]) &&
      boundedText(item.candidate, MATCH_LIMITS.maxSignalIceBytes) &&
      (item.sdpMid === null || boundedText(item.sdpMid,
        MATCH_LIMITS.maxSignalMetadataBytes, true)) &&
      (item.usernameFragment === null || boundedText(item.usernameFragment,
        MATCH_LIMITS.maxSignalMetadataBytes, true)) &&
      (item.sdpMLineIndex === null || (Number.isInteger(item.sdpMLineIndex) &&
        Number(item.sdpMLineIndex) >= 0 && Number(item.sdpMLineIndex) <= 255))) {
    return {...base, type: "webrtc_ice", candidate: item.candidate,
      sdpMid: item.sdpMid as string | null,
      sdpMLineIndex: item.sdpMLineIndex as number | null,
      usernameFragment: item.usernameFragment as string | null};
  }
  if (item.type === "peer_end" && exactKeys(item, ["reason"]) &&
      (item.reason === "restart" || item.reason === "close")) {
    return {...base, type: "peer_end", reason: item.reason};
  }
  return null;
}

/** A WebSocket is ordered, so exceeding either cap is terminal. The state is
 * serialized into the hibernatable socket attachment after every admission. */
export function admitMatchSignalMessage(state: MatchSignalRateState,
                                        now: number): boolean {
  if (!Number.isSafeInteger(now) || now < 0 || !Number.isSafeInteger(state.messages) ||
      state.messages < 0 || !Number.isSafeInteger(state.windowStartedAt) ||
      state.windowStartedAt < 0 || !Number.isSafeInteger(state.lifetimeMessages) ||
      state.lifetimeMessages < 0) return false;
  if (now < state.windowStartedAt) return false;
  if (now - state.windowStartedAt >= MATCH_LIMITS.signalWindowMs) {
    state.windowStartedAt = now;
    state.messages = 0;
  }
  if (state.messages >= MATCH_LIMITS.maxSignalMessagesPerWindow ||
      state.lifetimeMessages >= MATCH_LIMITS.maxSignalMessagesPerSocket) return false;
  state.messages++;
  state.lifetimeMessages++;
  return true;
}
