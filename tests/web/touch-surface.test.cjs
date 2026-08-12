"use strict";

const assert = require("node:assert/strict");
const path = require("node:path");

class Classes {
  constructor() { this.values = new Set(); }
  add(value) { this.values.add(value); }
  remove(value) { this.values.delete(value); }
  toggle(value, force) {
    if (force) this.values.add(value); else this.values.delete(value);
  }
  contains(value) { return this.values.has(value); }
}

class Element extends EventTarget {
  constructor(bit = null, rect = {left: 0, top: 0, right: 100, bottom: 100}) {
    super();
    this.dataset = bit == null ? {} : {touchButton: String(bit)};
    this.classList = new Classes();
    this.style = {};
    this.rect = rect;
    this.children = [];
    this.actions = null;
    this.attributes = new Map();
  }
  getBoundingClientRect() {
    return {...this.rect, width: this.rect.right - this.rect.left,
      height: this.rect.bottom - this.rect.top};
  }
  querySelectorAll(selector) {
    return selector === "[data-touch-button]" ? this.children : [];
  }
  querySelector(selector) {
    return selector === ".touch-actions" ? this.actions : null;
  }
  setPointerCapture() {}
  setAttribute(name, value) { this.attributes.set(name, String(value)); }
  getAttribute(name) { return this.attributes.get(name) ?? null; }
}

function pointer(type, id, x, y) {
  const event = new Event(type, {bubbles: true, cancelable: true});
  Object.assign(event, {pointerId: id, clientX: x, clientY: y,
    pointerType: "touch"});
  return event;
}

function keyboard(type, key) {
  const event = new Event(type, {bubbles: true, cancelable: true});
  Object.defineProperty(event, "key", {value: key});
  return event;
}

require(path.join(__dirname, "..", "..", "dist", "web", "input",
  "touch-surface.js"));

const win = new EventTarget();
const doc = new EventTarget();
doc.visibilityState = "visible";
const stick = new Element(null, {left: 0, top: 0, right: 200, bottom: 200});
const knob = new Element();
const actions = new Element();
const go = new Element(32768, {left: 0, top: 0, right: 80, bottom: 80});
const drift = new Element(16, {left: 100, top: 0, right: 180, bottom: 80});
const item = new Element(8192, {left: 0, top: 100, right: 80, bottom: 180});
const pause = new Element(4096, {left: 200, top: 0, right: 260, bottom: 60});
actions.children = [go, drift, item];
const controls = new Element();
controls.children = [go, drift, item, pause];
controls.actions = actions;
const state = {buttons: 0, stickX: 0, stickY: 0};
const snapshots = [];
const publish = () => snapshots.push({...state});
const clear = () => {
  state.buttons = 0; state.stickX = 0; state.stickY = 0; publish();
};
const surface = new globalThis.MDKRTouchSurface({
  controls, stick, knob, state, publish, clear, window: win, document: doc,
  haptics: false,
});

actions.dispatchEvent(pointer("pointerdown", 1, 40, 40));
assert.equal(state.buttons, 32768, "Go presses A immediately");
win.dispatchEvent(pointer("pointermove", 1, 140, 40));
assert.equal(state.buttons, 32768 | 16, "slide keeps A and adds Drift");
actions.dispatchEvent(pointer("pointerdown", 2, 40, 140));
assert.equal(state.buttons, 32768 | 16 | 8192,
  "second pointer forms Accelerate+Drift+Item chord");
win.dispatchEvent(pointer("pointerup", 2, 40, 140));
assert.equal(state.buttons, 32768 | 16);
win.dispatchEvent(pointer("pointercancel", 1, 140, 40));
assert.equal(state.buttons, 0);

stick.dispatchEvent(pointer("pointerdown", 3, 190, 100));
assert.ok(state.stickX > 70 && state.stickY === 0, "stick reaches full range");
win.dispatchEvent(pointer("pointercancel", 3, 190, 100));
assert.deepEqual({x: state.stickX, y: state.stickY}, {x: 0, y: 0});

stick.dispatchEvent(keyboard("keydown", "ArrowUp"));
assert.deepEqual({x: state.stickX, y: state.stickY}, {x: 0, y: 80},
  "arrow key reaches the full analog range");
assert.equal(stick.getAttribute("aria-valuetext"), "Up");
stick.dispatchEvent(keyboard("keydown", "d"));
assert.deepEqual({x: state.stickX, y: state.stickY}, {x: 57, y: 57},
  "keyboard diagonals are clamped to the analog radius");
assert.equal(stick.getAttribute("aria-valuetext"), "Up right");
stick.dispatchEvent(keyboard("keyup", "ArrowUp"));
assert.deepEqual({x: state.stickX, y: state.stickY}, {x: 80, y: 0});
stick.dispatchEvent(keyboard("keyup", "d"));
assert.deepEqual({x: state.stickX, y: state.stickY}, {x: 0, y: 0});
assert.equal(stick.getAttribute("aria-valuetext"), "Centered");

stick.dispatchEvent(keyboard("keydown", "a"));
stick.dispatchEvent(new Event("blur"));
assert.deepEqual({x: state.stickX, y: state.stickY}, {x: 0, y: 0},
  "losing keyboard focus synchronously neutralizes steering");

actions.dispatchEvent(pointer("pointerdown", 4, 40, 40));
doc.visibilityState = "hidden";
doc.dispatchEvent(new Event("visibilitychange"));
assert.deepEqual(state, {buttons: 0, stickX: 0, stickY: 0},
  "hidden lifecycle synchronously neutralizes every control");
assert.ok(snapshots.length >= 8);
surface.destroy();

console.log("touch_surface: PASS");
