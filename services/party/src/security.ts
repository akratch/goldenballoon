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
  return [...value.normalize("NFC").trim()]
    .slice(0, LIMITS.maxNameCodePoints).join("")
    .replace(/[\u0000-\u001f\u007f]/g, "");
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

export async function readJson<T>(request: Request, maxBytes = LIMITS.maxJsonBytes): Promise<T> {
  const declared = Number(request.headers.get("content-length") || "0");
  if (declared > maxBytes) throw new Response("request_too_large", {status: 413});
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (bytes.byteLength > maxBytes) throw new Response("request_too_large", {status: 413});
  try { return JSON.parse(new TextDecoder().decode(bytes)) as T; }
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

export function allowedOrigin(request: Request, env: Env): boolean {
  const origin = request.headers.get("origin");
  return origin === env.PARTY_ORIGIN;
}
