import {LIMITS, type Env} from "./types";

const encoder = new TextEncoder();

export function base64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function fromBase64Url(value: string): Uint8Array | null {
  if (!/^[A-Za-z0-9_-]{22,256}$/.test(value)) return null;
  try {
    const binary = atob(value.replace(/-/g, "+").replace(/_/g, "/") +
      "===".slice((value.length + 3) % 4));
    return Uint8Array.from(binary, character => character.charCodeAt(0));
  } catch { return null; }
}

export function randomToken(bytes: number): string {
  const value = new Uint8Array(bytes);
  crypto.getRandomValues(value);
  return base64Url(value);
}

export async function boundCredential(env: Env, purpose: string,
                                      roomId: string): Promise<string> {
  const nonce = crypto.getRandomValues(new Uint8Array(16));
  const nonceText = base64Url(nonce);
  const proof = fromBase64Url(await digest(env, `credential-binding-${purpose}`,
    `${roomId}\0${nonceText}`));
  if (!proof || proof.byteLength !== 32) throw new Error("credential binding failed");
  const token = new Uint8Array(32);
  token.set(nonce); token.set(proof.slice(0, 16), 16);
  return base64Url(token);
}

export async function validBoundCredential(env: Env, purpose: string,
                                           roomId: string,
                                           value: string): Promise<boolean> {
  const token = fromBase64Url(value);
  if (!token || token.byteLength !== 32) return false;
  const nonce = token.slice(0, 16);
  const proof = fromBase64Url(await digest(env, `credential-binding-${purpose}`,
    `${roomId}\0${base64Url(nonce)}`));
  if (!proof || proof.byteLength !== 32) return false;
  const expected = new Uint8Array(32);
  expected.set(nonce); expected.set(proof.slice(0, 16), 16);
  return constantTimeEqual(value, base64Url(expected));
}

export async function digest(env: Env, purpose: string, value: string): Promise<string> {
  if (!env.PARTY_HMAC_KEY || env.PARTY_HMAC_KEY.length < 32) {
    throw new Error("party secret is not provisioned");
  }
  const key = await crypto.subtle.importKey(
    "raw", encoder.encode(env.PARTY_HMAC_KEY),
    {name: "HMAC", hash: "SHA-256"}, false, ["sign"]);
  const signature = await crypto.subtle.sign(
    "HMAC", key, encoder.encode(`${purpose}\0${value}`));
  return base64Url(new Uint8Array(signature));
}

export function constantTimeEqual(left: string, right: string): boolean {
  if (left.length !== right.length || left.length > 256) return false;
  let difference = 0;
  for (let index = 0; index < left.length; index++) {
    difference |= left.charCodeAt(index) ^ right.charCodeAt(index);
  }
  return difference === 0;
}

export function normalizeName(value: unknown): string {
  if (typeof value !== "string") return "";
  const safe = value.normalize("NFC")
    .replace(/[\u0000-\u001f\u007f-\u009f\u200b-\u200f\u2028\u2029\u202a-\u202e\u2060\u2066-\u2069\ufeff]/g,
      "")
    .trim();
  return [...safe].slice(0, LIMITS.maxNameCodePoints).join("");
}

/** Return as soon as a JavaScript string's UTF-8 representation crosses the
 * wire limit. This deliberately avoids TextEncoder.encode(), which would
 * duplicate an attacker-controlled frame before it can be rejected. Lone
 * surrogates encode as the three-byte replacement character. */
export function utf8Exceeds(value: string, limit: number): boolean {
  let bytes = 0;
  for (let index = 0; index < value.length; index++) {
    const code = value.charCodeAt(index);
    if (code <= 0x7f) bytes += 1;
    else if (code <= 0x7ff) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff && index + 1 < value.length &&
             value.charCodeAt(index + 1) >= 0xdc00 &&
             value.charCodeAt(index + 1) <= 0xdfff) {
      bytes += 4;
      index++;
    } else bytes += 3;
    if (bytes > limit) return true;
  }
  return false;
}

export async function readRequestBytes(request: Request,
                                       maxBytes = LIMITS.maxJsonBytes):
    Promise<Uint8Array> {
  const length = request.headers.get("content-length");
  if (length !== null) {
    if (!/^\d+$/.test(length)) {
      throw new Response("invalid_content_length", {status: 400});
    }
    const declared = Number(length);
    if (!Number.isSafeInteger(declared) || declared > maxBytes) {
      throw new Response("request_too_large", {status: 413});
    }
  }
  if (!request.body) return new Uint8Array();
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    for (;;) {
      const {done, value} = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array) || total + value.byteLength > maxBytes) {
        try { await reader.cancel(); } catch { /* The 413 remains authoritative. */ }
        throw new Response("request_too_large", {status: 413});
      }
      total += value.byteLength;
      chunks.push(value);
    }
  } finally {
    try { reader.releaseLock(); } catch { /* A malformed stream stays rejected. */ }
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return bytes;
}

export async function readJson<T>(request: Request, maxBytes = LIMITS.maxJsonBytes): Promise<T> {
  const mediaType = request.headers.get("content-type") || "";
  if (!/^application\/json(?:\s*;|\s*$)/i.test(mediaType)) {
    throw new Response("unsupported_media_type", {status: 415});
  }
  const bytes = await readRequestBytes(request, maxBytes);
  try {
    const value: unknown = JSON.parse(
      new TextDecoder("utf-8", {fatal: true, ignoreBOM: true}).decode(bytes));
    /* Every service command is an object-shaped versioned protocol message.
     * Reject valid JSON primitives/arrays here so downstream field access
     * cannot turn an attacker-controlled shape error into a misleading 503. */
    if (!value || typeof value !== "object" || Array.isArray(value)) {
      throw new Response("invalid_json", {status: 400});
    }
    return value as T;
  }
  catch { throw new Response("invalid_json", {status: 400}); }
}

export function json(value: unknown, status = 200, extra?: HeadersInit): Response {
  const headers = new Headers(extra);
  headers.set("content-type", "application/json; charset=utf-8");
  headers.set("cache-control", "no-store");
  headers.set("referrer-policy", "no-referrer");
  headers.set("x-content-type-options", "nosniff");
  return Response.json(value, {status, headers});
}

function loopbackHostname(hostname: string): boolean {
  const value = hostname.toLowerCase();
  return value === "localhost" || value.endsWith(".localhost") ||
    value === "127.0.0.1" || value === "[::1]" || value === "::1";
}

/**
 * PARTY_ORIGIN is both the browser-origin authority boundary and the base used
 * to mint capability URLs. Require one canonical origin, not merely a string
 * which happens to equal an incoming Origin header. Production is HTTPS-only;
 * HTTP remains available solely for standards-defined loopback development.
 */
export function validPartyOrigin(value: unknown): value is string {
  if (typeof value !== "string" || value.length < 1 || value.length > 255) {
    return false;
  }
  try {
    const url = new URL(value);
    const trustedTransport = url.protocol === "https:" ||
      (url.protocol === "http:" && loopbackHostname(url.hostname));
    return trustedTransport && value === url.origin && !url.username &&
      !url.password && url.pathname === "/" && !url.search && !url.hash;
  } catch {
    return false;
  }
}

export function allowedOrigin(request: Request, env: Env): boolean {
  const origin = request.headers.get("origin");
  return validPartyOrigin(env.PARTY_ORIGIN) && origin === env.PARTY_ORIGIN;
}
