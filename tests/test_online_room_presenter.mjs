#!/usr/bin/env node
"use strict";

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {createRequire} from "node:module";
import {fileURLToPath} from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const presenter = require(path.join(
  root, "dist/web/online/online-room-presenter.js"));
const dump = process.argv[2];

assert.equal(presenter.version, 1, "presenter ABI changed without its consumer");
assert.ok(dump, "usage: test_online_room_presenter.mjs <browser-abi-test>");
const completed = spawnSync(dump, ["--dump-presenter-json"], {
  encoding: "utf8", timeout: 10_000, maxBuffer: 1024 * 1024,
});
assert.equal(completed.error, undefined,
  `could not execute browser model dump: ${completed.error}`);
assert.equal(completed.status, 0,
  `browser model dump failed:\n${completed.stderr || completed.stdout}`);
const models = JSON.parse(completed.stdout);
assert.equal(models.length, 43, "presenter did not receive all authoritative cases");

const actionInventory = new Set();
const renderedSignatures = new Set();
const phraseViews = [];
let timeoutViews = 0;
let failureViews = 0;
const failureInventory = new Set();
const liveRouteInventory = new Map();

function expectedControls(model) {
  const slots = model.timeoutVisible ? [3, 0, 1, 2] : [0, 1, 2];
  const seen = new Set();
  return slots.flatMap((slot) => {
    const control = model.controls[slot];
    if (!control.action || seen.has(control.action)) return [];
    seen.add(control.action);
    return [control];
  });
}

for (const model of models) {
  const view = presenter.project(model);
  const expected = expectedControls(model);
  assert.equal(view.slug, model.slug, `${model.slug}: slug drifted`);
  assert.equal(view.title, model.title, `${model.slug}: title drifted`);
  assert.equal(view.explanation, model.explanation,
    `${model.slug}: explanation drifted`);
  assert.equal(view.kind, model.kind, `${model.slug}: view kind drifted`);
  assert.equal(view.failure, model.failure, `${model.slug}: failure drifted`);
  assert.equal(view.verification.visible, Boolean(model.verificationPhrase),
    `${model.slug}: phrase visibility drifted`);
  assert.equal(view.verification.phrase, model.verificationPhrase,
    `${model.slug}: phrase copy drifted`);
  assert.deepEqual(view.actions.map(({action}) => action),
    expected.map(({action}) => action), `${model.slug}: action order drifted`);
  assert.deepEqual(view.actions.map(({label}) => label),
    expected.map(({label}) => label), `${model.slug}: action labels drifted`);
  assert.deepEqual(view.actions.map(({enabled}) => enabled),
    expected.map(({enabled}) => enabled), `${model.slug}: enabled state drifted`);
  assert.equal(view.timeout.visible, model.timeoutVisible,
    `${model.slug}: timeout visibility drifted`);
  assert.equal(view.timeout.title, model.timeoutTitle,
    `${model.slug}: timeout title drifted`);
  assert.equal(view.timeout.copy, model.timeoutCopy,
    `${model.slug}: timeout copy drifted`);
  assert.equal(view.announcement.priority,
    model.announcement === 2 ? "assertive" : "polite",
    `${model.slug}: announcement priority drifted`);
  assert.ok(view.announcement.text.startsWith(`${model.title}. ${model.explanation}`),
    `${model.slug}: announcement omits its visible heading or explanation`);
  assert.equal(view.countsText,
    `${model.members} ${model.members === 1 ? "member" : "members"} · ` +
    `${model.seats} ${model.seats === 1 ? "racer seat" : "racer seats"} · ` +
    `${model.ready} ready`, `${model.slug}: count grammar drifted`);
  assert.ok(Object.isFrozen(view) && Object.isFrozen(view.actions) &&
    Object.isFrozen(view.verification) && Object.isFrozen(view.timeout) &&
    Object.isFrozen(view.announcement), `${model.slug}: projection is mutable`);

  for (const item of view.actions) {
    actionInventory.add(item.action);
    assert.equal(item.destructive, item.action === 24 || item.action === 25,
      `${model.slug}: destructive tone drifted for action ${item.action}`);
    const expectsSelection = item.primary && [6, 7, 8].includes(item.action);
    assert.equal(Boolean(item.selection), expectsSelection,
      `${model.slug}: semantic selection control drifted`);
    if (item.selection) {
      assert.ok(Object.isFrozen(item.selection) &&
        Object.isFrozen(item.selection.options) &&
        item.selection.options.length >= 3,
      `${model.slug}: selection options are incomplete or mutable`);
    }
    const live = presenter.liveAction(view, item.action);
    assert.equal(Boolean(live), item.enabled,
      `${model.slug}: live action admission disagrees with rendered enabled state`);
    if (live) liveRouteInventory.set(item.action, live);
  }
  assert.equal(presenter.liveAction(view, 0), null,
    `${model.slug}: action zero entered live routing`);
  assert.equal(presenter.liveAction(view, 28), null,
    `${model.slug}: out-of-range action entered live routing`);
  const absent = Array.from({length: 27}, (_, index) => index + 1)
    .find((action) => !view.actions.some((item) => item.action === action));
  if (absent) assert.equal(presenter.liveAction(view, absent), null,
    `${model.slug}: unrendered action ${absent} entered live routing`);
  if (model.timeoutVisible) {
    timeoutViews++;
    assert.equal(view.actions[0]?.action, model.controls[3].action,
      `${model.slug}: expired action is not first`);
    assert.equal(view.actions[0]?.primary, true,
      `${model.slug}: expired action is not primary`);
  } else if (view.actions.length) {
    assert.equal(view.actions[0].primary, true,
      `${model.slug}: first model action is not primary`);
  }
  assert.equal(view.actions.filter(({primary}) => primary).length,
    view.actions.length ? 1 : 0,
    `${model.slug}: presentation must expose exactly one primary action`);
  if (model.failure !== 0) {
    failureViews++;
    failureInventory.add(model.failure);
    assert.equal(view.announcement.priority, "assertive",
      `${model.slug}: recovery is not announced assertively`);
  }
  if (model.verificationPhrase) {
    phraseViews.push({model, view});
    assert.ok(view.announcement.text.includes(
      `Verification phrase: ${model.verificationPhrase}.`) &&
      view.announcement.text.endsWith(
        "Do not continue if even 1 word differs."),
    `${model.slug}: phrase announcement omits comparison safety copy`);
  } else {
    assert.ok(!view.announcement.text.includes("Verification phrase:"),
      `${model.slug}: empty phrase was announced`);
  }
  const signature = JSON.stringify(view);
  assert.ok(!renderedSignatures.has(signature),
    `${model.slug}: semantic presentation duplicates another case`);
  renderedSignatures.add(signature);
}

assert.deepEqual([...actionInventory].sort((a, b) => a - b),
  Array.from({length: 27}, (_, index) => index + 1),
  "presenter does not expose all 27 typed actions");
assert.deepEqual([...liveRouteInventory.keys()].sort((a, b) => a - b),
  Array.from({length: 27}, (_, index) => index + 1),
  "live policy does not classify all 27 enabled typed actions");
for (const action of [11, 12, 13, 18, 22, 25, 26, 27]) {
  assert.equal(liveRouteInventory.get(action)?.available, false,
    `pre-admission live action ${action} must remain visibly locked`);
}
const setupView = presenter.project(models.find(({slug}) => slug === "room-friends"));
assert.deepEqual(presenter.liveAction(setupView, 4),
  {action: 4, route: "setup_locked", available: false},
  "production Check Setup must not fake a carrier/preflight result");
assert.deepEqual(presenter.liveAction(setupView, 4, {fixture: true}),
  {action: 4, route: "check_setup", available: true},
  "trusted browser evidence fixture lost its isolated setup path");
assert.equal(phraseViews.length, 1,
  "presenter must expose exactly one phrase-comparison view");
assert.ok(timeoutViews > 0, "presenter inventory has no local timeout view");
assert.deepEqual([...failureInventory].sort((a, b) => a - b),
  Array.from({length: 18}, (_, index) => index + 1),
  "presenter does not cover all 18 typed failures");

const phrase = phraseViews[0];
assert.equal(phrase.model.slug, "preflight", "phrase appears in the wrong journey");
assert.deepEqual(phrase.view.actions.slice(0, 2).map(({action, label}) =>
  [action, label]), [[26, "Words Match"], [27, "Words Differ"]],
"phrase view does not preserve both explicit decisions");
const mismatch = models.find(({slug}) => slug === "failure-verification-mismatch");
assert.ok(mismatch, "verification mismatch recovery is absent");
const mismatchView = presenter.project(mismatch);
assert.equal(mismatchView.failure, 18, "verification mismatch failure id drifted");
assert.equal(mismatchView.title, "Words Did Not Match",
  "verification mismatch title is not explicit");
assert.equal(mismatchView.actions[0]?.label, "Reconnect Securely",
  "verification mismatch lacks an actionable safe retry");
assert.equal(mismatchView.verification.visible, false,
  "verification mismatch leaves a stale phrase visible");
assert.equal(mismatchView.announcement.priority, "assertive",
  "verification mismatch is not assertive");

const local = models.find((model) => model.localPlay &&
  model.controls.some(({action}) => action === 21));
assert.ok(local, "gallery lacks a Play Here recovery projection");
const blockedLocal = presenter.project(local, {live: true, localPlayable: false});
assert.ok(blockedLocal.actions.some(({action, label}) =>
  action === 17 && label === "Choose ROM") &&
  !blockedLocal.actions.some(({action}) => action === 21),
"live recovery did not replace unavailable Play Here with Choose ROM");
const playableLocal = presenter.project(local, {live: true, localPlayable: true});
assert.ok(playableLocal.actions.some(({action, label}) =>
  action === 21 && label === "Play Here"),
"live recovery did not preserve available Play Here");
assert.equal(new Set(blockedLocal.actions.map(({action}) => action)).size,
  blockedLocal.actions.length, "live recovery duplicated a typed action");

for (const mutate of [
  (model) => ({...model, controls: model.controls.slice(0, 3)}),
  (model) => ({...model, members: 5}),
  (model) => ({...model, ready: model.members + 1}),
  (model) => ({...model, timeoutVisible: !model.timeoutVisible}),
  (model) => ({...model,
    verificationPhrase: "word word word"}),
  (model) => ({...model, controls: model.controls.map((control, index) =>
    index === 0 ? {...control, action: 28} : control)}),
  (model) => ({...model, localPlay: 1}),
]) {
  assert.throws(() => presenter.project(mutate(models[0])), TypeError,
    "presenter accepted a malformed shared projection");
}

console.log(`online room presenter tests passed: ${models.length} cases, ` +
  `${actionInventory.size} actions, ${failureInventory.size} failures`);
