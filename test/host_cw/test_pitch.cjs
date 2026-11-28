/* SPDX-License-Identifier: MIT */
// CW decoder, RIG: the pitch sent to the worker is the radio's CW pitch (GET /cat key_pitch_hz) when RIG is
// on and the pitch is known, otherwise the manual Hz value; a change of the radio's pitch sends exactly one
// config, and only when the pitch in use changes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = fs.readFileSync(require('node:path').join(__dirname, '../../main/index.html'), 'utf8');
const a = page.indexOf('var cwRadioPitch = null;');
const b = page.indexOf('// ---- end of RIG', a);
assert.ok(a >= 0 && b > a);

let configs = 0;
const context = vm.createContext({ cwSettings: { rig: true, auto: false, pitch: 700 } });
vm.runInContext(page.slice(a, b) + '\nthis.radio = () => cwRadioPitch;', context);
context.cwApply = () => { configs++; };
const set = (hz) => { const n = configs; context.cwSetRadioPitch(hz); return configs - n; };
const pitch = () => context.cwEffectivePitch();

assert.equal(pitch(), 700, 'RIG on, radio pitch not read yet: the manual value');
assert.equal(set(600), 1, 'radio pitch read: one config');
assert.equal(pitch(), 600, 'RIG on, radio pitch known: the radio pitch');
assert.equal(set(600), 0, 'the same radio pitch again: nothing');
assert.equal(set(650), 1, 'the radio pitch changes: one config');
assert.equal(set(null), 1, 'radio pitch lost (disconnected): back to the manual value');
assert.equal(pitch(), 700);
for (const bad of [299, 1210, 655.5, '600', undefined]) {
  assert.equal(set(bad), 0, `invalid pitch ${bad} is ignored`);
  assert.equal(context.radio(), null);
}
assert.equal(set(700), 0, 'radio pitch equal to the manual value: the pitch in use does not change');

context.cwSettings.rig = false;
assert.equal(pitch(), 700, 'RIG off: the manual value');
assert.equal(set(800), 0, 'RIG off: a new radio pitch sends nothing');
assert.equal(pitch(), 700);
context.cwSettings.rig = true;
assert.equal(pitch(), 800, 'RIG on again: the radio pitch read meanwhile');

context.cwSettings.auto = true;
assert.equal(set(900), 1, 'AUTO on: still one config (the worker ignores the pitch with AUTO)');
assert.equal(pitch(), 900);
console.log('CW decoder: RIG pitch (RIG on/off x pitch known/unknown x AUTO, one config per change)');
