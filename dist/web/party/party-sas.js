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

  async function phrase(privateKey, peerPublicKey, transcript) {
    const peer = await crypto.subtle.importKey("raw", decode(peerPublicKey),
      {name: "ECDH", namedCurve: "P-256"}, false, []);
    const secret = new Uint8Array(await crypto.subtle.deriveBits(
      {name: "ECDH", public: peer}, privateKey, 256));
    const context = encoder.encode([
      "golden-balloon-party-sas-v1", transcript.roomId,
      transcript.hostPublicKey, transcript.controllerPublicKey,
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

  root.MDKRPartySas = Object.freeze({createIdentity, phrase});
})(typeof globalThis !== "undefined" ? globalThis : this);
