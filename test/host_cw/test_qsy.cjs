/* SPDX-License-Identifier: MIT */
// CW decoder, new line at QSY: cwTrackRx sends one 'qsy' to the worker when a receiver that decodes CW
// changes mode or moves CW_QSY_BREAK_HZ or more (small tuning steps add up), and none otherwise.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = fs.readFileSync(require('node:path').join(__dirname, '../../main/index.html'), 'utf8');
const section = (from, to) => {
  const a = page.indexOf(from);
  const b = page.indexOf(to, a);
  assert.ok(a >= 0 && b > a, 'section ' + from);
  return page.slice(a, b);
};
const code = section('const CW_QSY_BREAK_HZ', '// ---- end of the QSY tracking');

const rx = [{ f: 0, m: '' }, { f: 0, m: '' }];
const sent = [];
const chan = (idx) => ({ idx, fixedHz: null, hz: null, rxFreq: 0, rxMode: '', pendingEl: { textContent: '' } });
const context = vm.createContext({
  cwReady: true,
  cwSettings: { on: [true, false] },
  cwChans: [chan(0), chan(1)],
  cwWorker: { postMessage: (m) => sent.push(m) },
  rxFreq: (idx) => rx[idx].f,
  rxMode: (idx) => rx[idx].m,
  cwShowTone: () => {},
});
vm.runInContext(code, context);
const poll = (f, m, idx = 0) => { rx[idx].f = f; rx[idx].m = m; const n = sent.length; context.cwTrackRx(); return sent.length - n; };

assert.equal(poll(0, 'CW-U'), 0, 'frequency 0 (radio not connected) is ignored');
assert.equal(context.cwChans[0].rxFreq, 0, 'frequency 0 is not kept');
assert.equal(poll(7025000, 'CW-U'), 0, 'the first valid frequency only starts tracking');
for (let i = 1; i <= 14; i++) assert.equal(poll(7025000 + 10 * i, 'CW-U'), 0, `fine tuning step ${i} (${10 * i} Hz)`);
assert.equal(poll(7025150, 'CW-U'), 1, '150 Hz in 10 Hz steps breaks the line once');
assert.equal(JSON.stringify(sent[sent.length - 1]), JSON.stringify({ type: 'qsy', ch: 0 }));
assert.equal(poll(7025160, 'CW-U'), 0, 'tracking restarts from the new frequency');
assert.equal(poll(7025160, 'CW-L'), 1, 'a mode change breaks the line');
assert.equal(poll(7025000, 'CW-L'), 1, 'a jump of 160 Hz down breaks the line');
assert.equal(poll(0, 'CW-L'), 0, 'disconnected: nothing sent');
assert.equal(context.cwChans[0].rxFreq, 7025000, 'and the kept frequency stays');

context.cwChans[0].fixedHz = 640;
assert.equal(poll(14025000, 'CW-L'), 1, 'QSY with a tone chosen on the scope');
assert.equal(context.cwChans[0].fixedHz, null, 'the tone chosen on the scope is released');

assert.equal(poll(14074000, 'CW-U', 1), 0, 'SUB first frequency');
assert.equal(poll(21074000, 'USB', 1), 0, 'SUB without CW sends nothing');
context.cwSettings.on[1] = true;
assert.equal(poll(21074000, 'USB', 1), 0, 'CW turned on: the first frequency only starts tracking');
assert.equal(poll(21075000, 'USB', 1), 1, 'then a QSY on SUB breaks its line');
assert.equal(JSON.stringify(sent[sent.length - 1]), JSON.stringify({ type: 'qsy', ch: 1 }));

context.cwReady = false;
assert.equal(poll(28000000, 'CW-U'), 0, 'nothing before the CW section is set up');
console.log('CW decoder: new line at QSY (mode, 150 Hz, small steps add up, frequency 0, CW off)');
