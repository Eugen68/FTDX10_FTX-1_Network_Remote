/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = fs.readFileSync(require('node:path').join(__dirname, '../../main/index.html'), 'utf8');
const section = (from, to) => {
  const a = page.indexOf(from);
  const b = page.indexOf(to, a);
  assert.ok(a >= 0 && b > a);
  return page.slice(a, b);
};
const catModeUpdates = section('      if (d.mode_main && document.activeElement !== modeSelectMain)', '      if (d.vol_main !== undefined)');
const code = section('const PREF_STEP_VALUES', 'function formatFreq') +
  section('function updateModeButtons(', '// CW follows') +
  `function reportCat(d) { const now = Date.now() + 2000; ${catModeUpdates} }`;

const selects = [false, true].map(() => ({ value: '', addEventListener(_, fn) { this.change = fn; },
  choose(value) { this.value = String(value); this.change({ target: this }); } }));
const groups = [{}, {}];
const prefs = Object.create(null), commands = [];
const context = vm.createContext({
  document: {
    getElementById(id) {
      if (id.startsWith('modeBtns')) return null;
      return (id.startsWith('stepSel') ? selects : groups)[id.endsWith('Sub') ? 1 : 0];
    },
  },
  uiPrefValues: prefs,
  queueUiPref: (name, value) => { prefs[name] = value; },
  sendCatCommand: command => commands.push(command),
  modeBadgeMain: {textContent: 'USB'}, modeBadgeSub: {textContent: 'AM'},
  modeSelectMain: {}, modeSelectSub: {}, lastUserChangeMain: 0, lastUserChangeSub: 0, USER_CHANGE_HOLD_MS: 1500,
});
vm.runInContext(code, context);
const check = (sub, expectedGroup, expectedStep) => {
  assert.equal(groups[Number(sub)].textContent, expectedGroup);
  assert.equal(context.getStep(sub), expectedStep);
  assert.equal(selects[Number(sub)].value, String(expectedStep));
};
check(false, 'SSB', 100);
check(true, 'AM', 1000);
context.setMode(false, 'CW-L');
check(false, 'CW', 10);
selects[0].choose(500);
check(false, 'CW', 500);
assert.equal(prefs.stepMainCW, 500);
context.reportCat({mode_main: 'DATA-FM-N', mode_sub: 'CW-U'});
check(false, 'FM', 12500);
check(true, 'CW', 10);
context.reportCat({mode_main: 'CW-U', mode_sub: 'RTTY-LSB'});
check(false, 'CW', 500);
check(true, 'DATA', 100);
context.reportCat({mode_main: 'C4FM-VW', mode_sub: 'AM-N'});
check(false, 'FM', 12500);
check(true, 'AM', 1000);
assert.equal(commands.length, 1);
assert.equal(commands[0].mode, 'CW-L');
console.log('STEP follows mode groups on page edits and CAT reports');
