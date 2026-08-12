"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const repo = path.resolve(__dirname, "..");
const source = fs.readFileSync(
  path.join(repo, "dist/web/party/qrcodegen.js"), "utf8");
const context = vm.createContext({});
vm.runInContext(source, context, {filename: "qrcodegen.js"});
const jsQR = require(path.join(repo, "services/party/node_modules/jsqr"));

const capability = "A".repeat(43);
const expected = `https://party.example.invalid/controller/#${capability}`;
const qr = context.qrcodegen.QrCode.encodeText(
  expected, context.qrcodegen.QrCode.Ecc.QUARTILE);
const quiet = 4;
const scale = 8;
const width = (qr.size + quiet * 2) * scale;
const pixels = new Uint8ClampedArray(width * width * 4);
for (let y = 0; y < width; y++) {
  for (let x = 0; x < width; x++) {
    const moduleX = Math.floor(x / scale) - quiet;
    const moduleY = Math.floor(y / scale) - quiet;
    const dark = moduleX >= 0 && moduleY >= 0 && moduleX < qr.size &&
      moduleY < qr.size && qr.getModule(moduleX, moduleY);
    const value = dark ? 0 : 255;
    const offset = (y * width + x) * 4;
    pixels[offset] = value;
    pixels[offset + 1] = value;
    pixels[offset + 2] = value;
    pixels[offset + 3] = 255;
  }
}
const decoded = jsQR(pixels, width, width, {inversionAttempts: "dontInvert"});
assert(decoded, "test-only decoder could not read generated QR");
assert.equal(decoded.data, expected);
console.log(`PASS: QR round-trip decoded ${qr.size}x${qr.size} modules at ECC Q`);
