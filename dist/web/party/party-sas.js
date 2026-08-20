// Ephemeral ECDH short-authentication string for display/phone verification.
(function (root) {
  "use strict";
  const LEFT = ["Amber", "Brave", "Bright", "Calm", "Coral", "Cosmic", "Daring",
    "Flying", "Gentle", "Golden", "Happy", "Icy", "Jolly", "Lucky", "Mighty",
    "Neon", "Nimble", "Orange", "Rapid", "Royal", "Silver", "Solar", "Sunny",
    "Swift", "Teal", "Tiny", "Turbo", "Velvet", "Violet", "Warm", "Wild", "Zippy"];
  const RIGHT = ["Balloon", "Comet", "Drum", "Falcon", "Fox", "Glider", "Kite",
    "Lion", "Moon", "Otter", "Panda", "Parrot", "Pebble", "Pilot", "Rocket",
    "Sail", "Sparrow", "Star", "Tiger", "Toucan", "Turtle", "Whale", "Wing",
    "Cloud", "Dolphin", "Lantern", "Meteor", "Penguin", "Planet", "Raven",
    "Sunrise", "Thunder"];
  const encoder = new TextEncoder();

  function base64Url(bytes) {
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }

  function decode(value) {
    if (!/^[A-Za-z0-9_-]{87}$/.test(value)) throw new Error("invalid_party_key");
    const binary = atob(value.replace(/-/g, "+").replace(/_/g, "/") + "=");
    const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
    if (bytes.byteLength !== 65 || bytes[0] !== 4) throw new Error("invalid_party_key");
    return bytes;
  }

  /* The browser only exposes crypto.subtle (and Wake Lock) on a secure
   * context. Local play serves this page over plain http on the LAN, where
   * subtle is absent, so every primitive it would have provided has a pure-JS
   * twin below. getSubtle() is read at call time so a secure page keeps using
   * the engine's vetted implementation and the test can force either path. */
  function getSubtle() {
    return (typeof crypto !== "undefined" && crypto) ? crypto.subtle : undefined;
  }

  /* =====================================================================
   * Pure-JS fallback: SHA-256 and P-256 ECDH.
   *
   * This is the ONLY place the SAS math is duplicated, and it exists solely
   * for insecure origins. It is deliberately small and auditable, depends on
   * nothing, and is pinned two ways in services/party/test/party-sas.test.ts:
   * the P-256 scalar multiply reproduces the published NIST CAVP ECDH vector,
   * and the whole fallback is proved byte-identical to crypto.subtle on random
   * inputs and to the 5a oracle vectors. It is NOT constant-time -- one
   * derivation happens per pairing, entirely inside the phone, with no remote
   * observer of the JS timing, so correctness and readability win over the
   * masking a side-channel-hardened ladder would add.
   * ===================================================================== */

  /* ---- SHA-256 (FIPS 180-4) ---- */
  const SHA_K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);

  function rotr(value, bits) {
    return ((value >>> bits) | (value << (32 - bits))) >>> 0;
  }

  function sha256Bytes(message) {
    const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const length = message.length;
    const total = (Math.floor((length + 8) / 64) + 1) * 64;
    const buffer = new Uint8Array(total);
    buffer.set(message);
    buffer[length] = 0x80;
    const view = new DataView(buffer.buffer);
    const bitLength = length * 8;
    view.setUint32(total - 8, Math.floor(bitLength / 0x100000000));
    view.setUint32(total - 4, bitLength >>> 0);
    const w = new Uint32Array(64);
    for (let offset = 0; offset < total; offset += 64) {
      for (let i = 0; i < 16; i++) w[i] = view.getUint32(offset + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
      }
      let a = h[0], b = h[1], c = h[2], d = h[3];
      let e = h[4], f = h[5], g = h[6], hh = h[7];
      for (let i = 0; i < 64; i++) {
        const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const ch = (e & f) ^ (~e & g);
        const t1 = (hh + S1 + ch + SHA_K[i] + w[i]) >>> 0;
        const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) >>> 0;
        hh = g; g = f; f = e; e = (d + t1) >>> 0;
        d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0;
      h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
      h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0;
      h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
    }
    const out = new Uint8Array(32);
    const outView = new DataView(out.buffer);
    for (let i = 0; i < 8; i++) outView.setUint32(i * 4, h[i]);
    return out;
  }

  /* ---- P-256 (secp256r1) scalar multiplication, Jacobian coordinates ---- */
  const FIELD =
    0xffffffff00000001000000000000000000000000ffffffffffffffffffffffffn;
  const ORDER =
    0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n;
  const CURVE_B =
    0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604bn;
  const GENERATOR_X =
    0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296n;
  const GENERATOR_Y =
    0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5n;

  function fieldMod(x) {
    const r = x % FIELD;
    return r >= 0n ? r : r + FIELD;
  }

  function fieldPow(base, exponent) {
    let result = 1n;
    base = fieldMod(base);
    while (exponent > 0n) {
      if (exponent & 1n) result = (result * base) % FIELD;
      base = (base * base) % FIELD;
      exponent >>= 1n;
    }
    return result;
  }

  const fieldInverse = (x) => fieldPow(x, FIELD - 2n);

  /* Point doubling for a = -3 (EFD dbl-2004-hmv). Point is [X, Y, Z];
   * the point at infinity is any triple with Z === 0n. */
  function jacobianDouble(point) {
    const [x1, y1, z1] = point;
    if (z1 === 0n) return [0n, 1n, 0n];
    const delta = (z1 * z1) % FIELD;
    const gamma = (y1 * y1) % FIELD;
    const beta = (x1 * gamma) % FIELD;
    const alpha = fieldMod(3n * fieldMod(x1 - delta) * fieldMod(x1 + delta));
    const x3 = fieldMod(alpha * alpha - 8n * beta);
    const z3 = fieldMod(fieldMod((y1 + z1) * (y1 + z1)) - gamma - delta);
    const y3 = fieldMod(alpha * fieldMod(4n * beta - x3) - 8n * gamma * gamma);
    return [x3, y3, z3];
  }

  /* General Jacobian addition (EFD add-2007-bl). */
  function jacobianAdd(a, b) {
    if (a[2] === 0n) return b;
    if (b[2] === 0n) return a;
    const [x1, y1, z1] = a;
    const [x2, y2, z2] = b;
    const z1z1 = (z1 * z1) % FIELD;
    const z2z2 = (z2 * z2) % FIELD;
    const u1 = (x1 * z2z2) % FIELD;
    const u2 = (x2 * z1z1) % FIELD;
    const s1 = fieldMod(y1 * z2 * z2z2);
    const s2 = fieldMod(y2 * z1 * z1z1);
    if (u1 === u2) {
      if (s1 !== s2) return [0n, 1n, 0n];
      return jacobianDouble(a);
    }
    const h = fieldMod(u2 - u1);
    const i = fieldMod(4n * h * h);
    const j = (h * i) % FIELD;
    const r = fieldMod(2n * (s2 - s1));
    const v = (u1 * i) % FIELD;
    const x3 = fieldMod(r * r - j - 2n * v);
    const y3 = fieldMod(r * fieldMod(v - x3) - 2n * s1 * j);
    const z3 = fieldMod(fieldMod((z1 + z2) * (z1 + z2) - z1z1 - z2z2) * h);
    return [x3, y3, z3];
  }

  function scalarMultiply(scalar, point) {
    let result = [0n, 1n, 0n];
    for (let bit = 255n; bit >= 0n; bit--) {
      result = jacobianDouble(result);
      if ((scalar >> bit) & 1n) result = jacobianAdd(result, point);
    }
    return result;
  }

  function toAffine(point) {
    if (point[2] === 0n) return null;
    const zInverse = fieldInverse(point[2]);
    const zInverse2 = (zInverse * zInverse) % FIELD;
    const zInverse3 = (zInverse2 * zInverse) % FIELD;
    return [(point[0] * zInverse2) % FIELD, (point[1] * zInverse3) % FIELD];
  }

  function bytesToBigInt(bytes) {
    let value = 0n;
    for (const byte of bytes) value = (value << 8n) | BigInt(byte);
    return value;
  }

  function bigIntTo32Bytes(value) {
    const out = new Uint8Array(32);
    for (let i = 31; i >= 0; i--) {
      out[i] = Number(value & 0xffn);
      value >>= 8n;
    }
    return out;
  }

  function isOnCurve(x, y) {
    return fieldMod(y * y) === fieldMod(x * x * x - 3n * x + CURVE_B);
  }

  /* ECDH shared secret: the affine x-coordinate of scalar*peer, big-endian and
   * left-padded to 32 bytes -- byte-for-byte what crypto.subtle.deriveBits
   * returns for P-256. The peer point is validated on-curve first (importKey
   * does the same), so a bogus key refuses rather than derives. */
  function ecdhSecretBytes(scalarBytes, peerBytes) {
    const scalar = bytesToBigInt(scalarBytes);
    const px = bytesToBigInt(peerBytes.subarray(1, 33));
    const py = bytesToBigInt(peerBytes.subarray(33, 65));
    if (!isOnCurve(px, py)) throw new Error("invalid_party_key");
    const shared = toAffine(scalarMultiply(scalar, [px, py, 1n]));
    if (!shared) throw new Error("invalid_party_key");
    return bigIntTo32Bytes(shared[0]);
  }

  function createIdentityFallback() {
    let scalar;
    let scalarBytes;
    do {
      scalarBytes = new Uint8Array(32);
      crypto.getRandomValues(scalarBytes);
      scalar = bytesToBigInt(scalarBytes);
    } while (scalar < 1n || scalar >= ORDER);
    const point = toAffine(scalarMultiply(scalar, [GENERATOR_X, GENERATOR_Y, 1n]));
    const publicBytes = new Uint8Array(65);
    publicBytes[0] = 4;
    publicBytes.set(bigIntTo32Bytes(point[0]), 1);
    publicBytes.set(bigIntTo32Bytes(point[1]), 33);
    return Object.freeze({privateKey: scalarBytes, publicKey: base64Url(publicBytes)});
  }

  /* ---- Shared primitives, routed to subtle or the fallback ---- */
  async function digestSha256(bytes) {
    const subtle = getSubtle();
    if (subtle) return new Uint8Array(await subtle.digest("SHA-256", bytes));
    return sha256Bytes(bytes);
  }

  async function sharedSecret(privateKey, peerPublicKey) {
    const peerBytes = decode(peerPublicKey);
    const subtle = getSubtle();
    if (subtle) {
      const peer = await subtle.importKey("raw", peerBytes,
        {name: "ECDH", namedCurve: "P-256"}, false, []);
      return new Uint8Array(await subtle.deriveBits(
        {name: "ECDH", public: peer}, privateKey, 256));
    }
    return ecdhSecretBytes(privateKey, peerBytes);
  }

  async function createIdentity() {
    const subtle = getSubtle();
    if (!subtle) return createIdentityFallback();
    const keyPair = await subtle.generateKey(
      {name: "ECDH", namedCurve: "P-256"}, false, ["deriveBits"]);
    const publicBytes = new Uint8Array(await subtle.exportKey("raw", keyPair.publicKey));
    return Object.freeze({privateKey: keyPair.privateKey,
      publicKey: base64Url(publicBytes)});
  }

  /* Fingerprint canonicalization, agreed byte-for-byte with the native
   * transport: the value after a line BEGINNING with "a=fingerprint:",
   * tokens re-joined with a single space, algorithm token verbatim, hex
   * uppercased, e.g. "sha-256 AB:CD:…". A line that begins with the
   * attribute but does not parse, or two lines that disagree, refuse the
   * whole description. Refusal is the empty string: no fingerprint can
   * only ever mean no phrase. */
  function canonicalFingerprintValue(line) {
    const tokens = line.split(/[ \t]+/).filter(Boolean);
    if (tokens.length !== 2) return "";
    const [algorithm, value] = tokens;
    if (algorithm.length > 32 || value.length > 512 ||
        !/^[A-Za-z0-9-]+$/.test(algorithm) ||
        !/^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2})*$/.test(value)) return "";
    return algorithm + " " + value.toUpperCase();
  }

  function sdpFingerprint(sdp) {
    let canonical = "";
    for (const rawLine of String(sdp || "").split("\n")) {
      const line = rawLine.endsWith("\r") ? rawLine.slice(0, -1) : rawLine;
      if (!line.startsWith("a=fingerprint:")) continue;
      const value = canonicalFingerprintValue(line.slice(14));
      if (!value || (canonical && canonical !== value)) return "";
      canonical = value;
    }
    return canonical;
  }

  /* SAS v2: the phrase commits to both DTLS fingerprints, so a relay that
   * substituted either certificate moves the words on one screen. Missing
   * fingerprints refuse outright — there is no v1 transcript to fall back
   * to, so "no fingerprint" can only ever mean "no phrase" (fail closed).
   * The transcript and word mapping live here once; only the ECDH and the
   * digest differ between the secure and insecure paths, and both are proved
   * to agree bit-for-bit in the tests. */
  async function phrase(privateKey, peerPublicKey, transcript) {
    const hostFingerprint = String(transcript.hostFingerprint || "");
    const controllerFingerprint = String(transcript.controllerFingerprint || "");
    if (!hostFingerprint || !controllerFingerprint) {
      throw new Error("missing_party_fingerprint");
    }
    const secret = await sharedSecret(privateKey, peerPublicKey);
    const context = encoder.encode([
      "golden-balloon-party-sas-v2", transcript.roomId,
      transcript.hostPublicKey, transcript.controllerPublicKey,
      hostFingerprint, controllerFingerprint,
    ].join("\0"));
    const material = new Uint8Array(secret.byteLength + context.byteLength);
    material.set(secret); material.set(context, secret.byteLength);
    const result = await digestSha256(material);
    /* Twenty displayed comparison bits. Each whitespace-delimited token is a
     * memorable compound drawn from a 32x32 namespace; two ordinary 32-word
     * lists without the compounds would expose only ten bits and be cheap for
     * an active rendezvous attacker to brute-force. */
    const value = (result[0] << 12) | (result[1] << 4) | (result[2] >>> 4);
    return `${LEFT[(value >>> 15) & 31]}-${RIGHT[(value >>> 10) & 31]} ` +
      `${LEFT[(value >>> 5) & 31]}-${RIGHT[value & 31]}`;
  }

  /* `fallback` is the pure-JS crypto the insecure-origin path runs; it is
   * surfaced so the tests can pin SHA-256 and P-256 ECDH directly against
   * published FIPS/NIST vectors, independently of the SAS transcript. */
  root.MDKRPartySas = Object.freeze({createIdentity, phrase, sdpFingerprint,
    fallback: Object.freeze({sha256: sha256Bytes, ecdh: ecdhSecretBytes})});
})(typeof globalThis !== "undefined" ? globalThis : this);
