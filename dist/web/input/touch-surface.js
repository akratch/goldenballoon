// Shared, transport-agnostic analog/multi-touch controller surface.
(function (root) {
  "use strict";

  class TouchSurface {
    constructor(options) {
      if (!options || !options.controls || !options.stick || !options.knob ||
          !options.state || typeof options.publish !== "function") {
        throw new TypeError("TouchSurface requires controls, stick, knob, state and publish");
      }
      this.controls = options.controls;
      this.stick = options.stick;
      this.knob = options.knob;
      this.state = options.state;
      this.publish = options.publish;
      this.clear = typeof options.clear === "function" ? options.clear : () => {};
      this.window = options.window || root;
      this.document = options.document || root.document;
      this.maxStick = Number.isFinite(options.maxStick) ? options.maxStick : 80;
      this.deadzone = Number.isFinite(options.deadzone) ? options.deadzone : 0.08;
      this.haptics = options.haptics !== false;
      this.stickPointer = null;
      this.stickKeys = new Set();
      this.accessibilityButtons = 0;
      this.pressedPointers = new Map();
      this.pressedClasses = new Set();
      this.pulseTimers = new Map();
      this.listeners = [];
      this.actionButtons = [...this.controls.querySelectorAll("[data-touch-button]")];
      this.actions = this.controls.querySelector(".touch-actions");
      this.clusterButtons = this.actions
        ? [...this.actions.querySelectorAll("[data-touch-button]")] : [];
      this.N64_A = 32768;
      this.N64_BRAKE = 16384;
      this.N64_PAUSE = 4096;
      this.wire();
    }

    listen(target, name, listener, options) {
      target.addEventListener(name, listener, options);
      this.listeners.push(() => target.removeEventListener(name, listener, options));
    }

    recomputeButtons() {
      let buttons = this.accessibilityButtons;
      this.pressedPointers.forEach((press) => { buttons |= press.bits; });
      this.state.buttons = buttons >>> 0;
      this.publish();
    }

    refreshButtonClasses() {
      this.actionButtons.forEach((button) => {
        const bit = Number(button.dataset.touchButton) >>> 0;
        const active = (this.accessibilityButtons & bit) !== 0 ||
          [...this.pressedPointers.values()].some(
            (press) => (press.bits & bit) === bit);
        button.classList.toggle("is-pressed", active);
        if (active) this.pressedClasses.add(button);
        else this.pressedClasses.delete(button);
      });
    }

    releaseButtonPointer(pointerId) {
      if (!this.pressedPointers.delete(pointerId)) return;
      this.refreshButtonClasses();
      this.recomputeButtons();
    }

    resetStick() {
      this.stickPointer = null;
      this.state.stickX = 0;
      this.state.stickY = 0;
      this.knob.style.transform = "translate3d(0, 0, 0)";
      this.stick.classList.remove("is-active");
      this.stick.setAttribute("aria-valuetext", "Centered");
      this.publish();
    }

    releaseAll() {
      this.pressedPointers.clear();
      this.accessibilityButtons = 0;
      this.stickKeys.clear();
      this.pulseTimers.forEach((timer) => clearTimeout(timer));
      this.pulseTimers.clear();
      this.pressedClasses.forEach((element) => element.classList.remove("is-pressed"));
      this.pressedClasses.clear();
      this.resetStick();
      this.clear();
    }

    updateStick(event) {
      const rect = this.stick.getBoundingClientRect();
      const centerX = rect.left + rect.width / 2;
      const centerY = rect.top + rect.height / 2;
      const radius = Math.max(24, Math.min(rect.width, rect.height) * 0.31);
      let dx = event.clientX - centerX;
      let dy = event.clientY - centerY;
      const distance = Math.hypot(dx, dy);
      if (distance > radius) {
        dx = dx * radius / distance;
        dy = dy * radius / distance;
      }
      const normalized = Math.min(1, Math.hypot(dx, dy) / radius);
      const magnitude = normalized <= this.deadzone
        ? 0 : (normalized - this.deadzone) / (1 - this.deadzone);
      const angle = Math.atan2(dy, dx);
      this.state.stickX = Math.round(
        Math.cos(angle) * magnitude * this.maxStick);
      this.state.stickY = Math.round(
        -Math.sin(angle) * magnitude * this.maxStick);
      this.knob.style.transform =
        `translate3d(${dx.toFixed(1)}px, ${dy.toFixed(1)}px, 0)`;
      this.publish();
    }

    updateKeyboardStick() {
      const pressed = (...keys) => keys.some((key) => this.stickKeys.has(key));
      let x = Number(pressed("arrowright", "d")) -
        Number(pressed("arrowleft", "a"));
      let y = Number(pressed("arrowup", "w")) -
        Number(pressed("arrowdown", "s"));
      if (x && y) {
        x *= Math.SQRT1_2;
        y *= Math.SQRT1_2;
      }
      this.state.stickX = Math.round(x * this.maxStick);
      this.state.stickY = Math.round(y * this.maxStick);
      const rect = this.stick.getBoundingClientRect();
      const radius = Math.max(24, Math.min(rect.width, rect.height) * 0.31);
      this.knob.style.transform =
        `translate3d(${(x * radius).toFixed(1)}px, ${(-y * radius).toFixed(1)}px, 0)`;
      this.stick.classList.toggle("is-active", Boolean(x || y));
      const vertical = y > 0 ? "Up" : (y < 0 ? "Down" : "");
      const horizontal = x > 0 ? "right" : (x < 0 ? "left" : "");
      const direction = [vertical, horizontal].filter(Boolean).join(" ");
      this.stick.setAttribute("aria-valuetext", direction || "Centered");
      this.publish();
    }

    chordBits(bit) {
      if (bit === this.N64_A || bit === this.N64_BRAKE) return bit;
      return bit | this.N64_A;
    }

    padRects() {
      return this.clusterButtons.map((button) => ({
        button,
        bit: Number(button.dataset.touchButton) >>> 0,
        rect: button.getBoundingClientRect(),
      }));
    }

    refreshRects() {
      this.pressedPointers.forEach((press) => {
        if (press.rects) press.rects = this.padRects();
      });
    }

    zoneAt(rects, x, y, reach = 14) {
      let best = null;
      let bestScore = Infinity;
      for (const entry of rects) {
        const rect = entry.rect;
        const dx = x < rect.left ? rect.left - x :
          (x > rect.right ? x - rect.right : 0);
        const dy = y < rect.top ? rect.top - y :
          (y > rect.bottom ? y - rect.bottom : 0);
        const outside = Math.max(dx, dy);
        if (outside > reach) continue;
        const cx = x - (rect.left + rect.right) / 2;
        const cy = y - (rect.top + rect.bottom) / 2;
        const score = outside * 1e7 + cx * cx + cy * cy;
        if (score < bestScore) { bestScore = score; best = entry; }
      }
      return best;
    }

    vibrate(milliseconds, pointerType) {
      if (!this.haptics || pointerType === "mouse") return;
      try {
        if (navigator.vibrate) navigator.vibrate(milliseconds);
      } catch (_) {}
    }

    wire() {
      this.listen(this.stick, "pointerdown", (event) => {
        if (this.stickPointer !== null) return;
        event.preventDefault();
        this.stickKeys.clear();
        this.stickPointer = event.pointerId;
        try { this.stick.setPointerCapture(event.pointerId); } catch (_) {}
        this.stick.classList.add("is-active");
        this.updateStick(event);
      });
      this.listen(this.window, "pointermove", (event) => {
        if (event.pointerId !== this.stickPointer) return;
        event.preventDefault();
        this.updateStick(event);
      }, true);
      const endStick = (event) => {
        if (event.pointerId === this.stickPointer) this.resetStick();
      };
      this.listen(this.window, "pointerup", endStick, true);
      this.listen(this.window, "pointercancel", endStick, true);

      const stickKey = (event) => {
        const key = String(event.key || "").toLowerCase();
        return ["arrowleft", "arrowright", "arrowup", "arrowdown",
          "a", "d", "w", "s"].includes(key) ? key : null;
      };
      this.listen(this.stick, "keydown", (event) => {
        const key = stickKey(event);
        if (!key || this.stickPointer !== null) return;
        event.preventDefault();
        if (this.stickKeys.has(key)) return;
        this.stickKeys.add(key);
        this.updateKeyboardStick();
      });
      this.listen(this.stick, "keyup", (event) => {
        const key = stickKey(event);
        if (!key || !this.stickKeys.has(key)) return;
        event.preventDefault();
        this.stickKeys.delete(key);
        this.updateKeyboardStick();
      });
      this.listen(this.stick, "blur", () => {
        if (!this.stickKeys.size || this.stickPointer !== null) return;
        this.stickKeys.clear();
        this.resetStick();
      });

      if (this.actions) {
        this.listen(this.actions, "pointerdown", (event) => {
          const rects = this.padRects();
          const zone = this.zoneAt(
            rects, event.clientX, event.clientY, Infinity);
          if (!zone) return;
          event.preventDefault();
          try { this.actions.setPointerCapture(event.pointerId); } catch (_) {}
          this.pressedPointers.set(event.pointerId, {
            rects,
            bits: this.chordBits(zone.bit),
            zone: zone.button,
          });
          this.refreshButtonClasses();
          this.recomputeButtons();
          this.vibrate(8, event.pointerType);
        });
      }
      this.listen(this.window, "pointermove", (event) => {
        const press = this.pressedPointers.get(event.pointerId);
        if (!press || !press.rects) return;
        const zone = this.zoneAt(press.rects, event.clientX, event.clientY);
        if (!zone || zone.button === press.zone) return;
        press.zone = zone.button;
        press.bits = this.chordBits(zone.bit);
        this.refreshButtonClasses();
        this.recomputeButtons();
        this.vibrate(5, event.pointerType);
      }, true);
      for (const name of ["pointerup", "pointercancel"]) {
        this.listen(this.window, name,
          (event) => this.releaseButtonPointer(event.pointerId), true);
      }

      this.actionButtons.forEach((button) => {
        const bit = Number(button.dataset.touchButton) >>> 0;
        if (bit === this.N64_PAUSE) {
          this.listen(button, "pointerdown", (event) => {
            event.preventDefault();
            try { button.setPointerCapture(event.pointerId); } catch (_) {}
            this.pressedPointers.set(event.pointerId, {bits: bit, zone: button});
            this.refreshButtonClasses();
            this.recomputeButtons();
            this.vibrate(8, event.pointerType);
          });
        }
        this.listen(button, "click", (event) => {
          if (event.detail !== 0) return;
          const pulse = bit === this.N64_PAUSE ? bit : this.chordBits(bit);
          const prior = this.pulseTimers.get(bit);
          if (prior) clearTimeout(prior);
          this.accessibilityButtons |= pulse;
          button.classList.add("is-pressed");
          this.recomputeButtons();
          this.pulseTimers.set(bit, setTimeout(() => {
            this.accessibilityButtons &= ~pulse;
            this.pulseTimers.delete(bit);
            this.refreshButtonClasses();
            this.recomputeButtons();
          }, 90));
        });
      });
      this.listen(this.window, "blur", () => this.releaseAll());
      this.listen(this.window, "pagehide", () => this.releaseAll());
      this.listen(this.document, "visibilitychange", () => {
        if (this.document.visibilityState !== "visible") this.releaseAll();
      });
      this.listen(this.document, "fullscreenchange", () => this.releaseAll());
    }

    destroy() {
      this.releaseAll();
      this.listeners.splice(0).forEach((remove) => remove());
    }
  }

  root.MDKRTouchSurface = TouchSurface;
})(typeof globalThis !== "undefined" ? globalThis : this);
