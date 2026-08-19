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

  async function createIdentity() {
    const keyPair = await crypto.subtle.generateKey(
      {name: "ECDH", namedCurve: "P-256"}, false, ["deriveBits"]);
    const publicBytes = new Uint8Array(await crypto.subtle.exportKey("raw", keyPair.publicKey));
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
   * to, so "no fingerprint" can only ever mean "no phrase" (fail closed). */
  async function phrase(privateKey, peerPublicKey, transcript) {
    const hostFingerprint = String(transcript.hostFingerprint || "");
    const controllerFingerprint = String(transcript.controllerFingerprint || "");
    if (!hostFingerprint || !controllerFingerprint) {
      throw new Error("missing_party_fingerprint");
    }
    const peer = await crypto.subtle.importKey("raw", decode(peerPublicKey),
      {name: "ECDH", namedCurve: "P-256"}, false, []);
    const secret = new Uint8Array(await crypto.subtle.deriveBits(
      {name: "ECDH", public: peer}, privateKey, 256));
    const context = encoder.encode([
      "golden-balloon-party-sas-v2", transcript.roomId,
      transcript.hostPublicKey, transcript.controllerPublicKey,
      hostFingerprint, controllerFingerprint,
    ].join("\0"));
    const material = new Uint8Array(secret.byteLength + context.byteLength);
    material.set(secret); material.set(context, secret.byteLength);
    const result = new Uint8Array(await crypto.subtle.digest("SHA-256", material));
    /* Twenty displayed comparison bits. Each whitespace-delimited token is a
     * memorable compound drawn from a 32x32 namespace; two ordinary 32-word
     * lists without the compounds would expose only ten bits and be cheap for
     * an active rendezvous attacker to brute-force. */
    const value = (result[0] << 12) | (result[1] << 4) | (result[2] >>> 4);
    return `${LEFT[(value >>> 15) & 31]}-${RIGHT[(value >>> 10) & 31]} ` +
      `${LEFT[(value >>> 5) & 31]}-${RIGHT[value & 31]}`;
  }

  root.MDKRPartySas = Object.freeze({createIdentity, phrase, sdpFingerprint});
})(typeof globalThis !== "undefined" ? globalThis : this);
