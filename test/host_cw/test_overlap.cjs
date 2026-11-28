/* SPDX-License-Identifier: MIT */
// CW decoder, overlapping windows: the step, the CTC tokens and the merger, taken from the page's worker
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
const code = section('const CW_RATE = 9600;', 'const CW_BIN_HZ') +
  section('const CW_VOCAB', '// Streaming:') +
  section('const CW_WIN = ', '// ---- end of overlapping windows') +
  'this.api = { CW_RATE, CW_OUT_HOP, CW_VOCAB, CW_BLANK, CW_SPACE, CW_WIN, CW_EDGE_GUARD, CW_TOLERANCE, CW_STEP_S, cwHop, cwTokens, cwTokText, CwMerger };';
// the vm's arrays come from another realm: compare plain copies
const same = (a, b, msg) => assert.deepEqual(JSON.parse(JSON.stringify(a)), JSON.parse(JSON.stringify(b)), msg);
const ctx = vm.createContext({});
vm.runInContext(code, ctx);
const { CW_RATE: SR, CW_OUT_HOP, CW_VOCAB, CW_BLANK, CW_SPACE, CW_WIN, CW_EDGE_GUARD, CW_TOLERANCE, CW_STEP_S,
        cwHop, cwTokens, cwTokText, CwMerger } = ctx.api;

// 1. The step, on the model's 288-sample output grid
assert.equal(CW_OUT_HOP, 288);
same([2, 4, 8].map(cwHop), [19296, 38304, 76896]);
for (const s of [1, 2, 3, 4, 5, 8, 12, 20]) assert.equal(cwHop(s) % 288, 0);
assert.equal(cwHop(100), Math.floor(CW_WIN / 288) * 288);   // at most a window
assert.equal(cwHop(0), 288);                                  // at least a frame

// 2. Port of SameTextForEveryChoiceAndFasterConfirmationWithSmallerSteps: every choice confirms the same
// text, and smaller steps confirm it sooner
const stream = Array.from({ length: 300 }, (_, i) => ({ s: Math.floor(i * 0.3 * SR), i: 1 + i % 26, p: 0.9 }));
const audioLength = 90 * SR;
const results = {};
for (const choice of ['fast', 'normal', 'precise']) {
  const hop = cwHop(CW_STEP_S[choice]);
  const merger = new CwMerger(CW_EDGE_GUARD, CW_TOLERANCE);
  const committed = [], delays = [];
  for (let start = 0; start + CW_WIN <= audioLength; start += hop) {
    const end = start + CW_WIN;
    for (const t of merger.push(start, CW_WIN, stream.filter((t) => t.s >= start && t.s < end).map((t) => ({ ...t })))) {
      committed.push(t);
      delays.push((end - t.s) / SR);
    }
  }
  delays.sort((a, b) => a - b);
  results[choice] = { text: committed, median: delays[delays.length >> 1] };
}
const until = 70 * SR;
const expected = stream.filter((t) => t.s < until);
for (const [choice, r] of Object.entries(results)) {
  same(r.text.filter((t) => t.s < until).map((t) => [t.s, t.i]), expected.map((t) => [t.s, t.i]), choice);
}
assert.ok(results.fast.median < results.normal.median, 'Rapid confirms before Normal');
assert.ok(results.normal.median < results.precise.median, 'Normal confirms before Precise');
assert.ok(results.normal.median >= 5 && results.normal.median <= 9, 'Normal median ' + results.normal.median);
assert.ok(results.precise.median >= 10 && results.precise.median <= 16, 'Precise median ' + results.precise.median);

// 3. A character seen by two windows comes out once, the more confident copy; a token inside the edge
// guard at the start of a later window is ignored
{
  const m = new CwMerger(CW_EDGE_GUARD, CW_TOLERANCE);
  const hop = cwHop(4);
  const at = 6 * SR;   // inside both windows' reliable parts
  same(m.push(0, CW_WIN, [{ s: at, i: 5, p: 0.6 }, { s: 11.5 * SR, i: 7, p: 0.9 }]), []);
  const out = m.push(hop, CW_WIN, [
    { s: hop + 0.5 * SR, i: 9, p: 0.99 },   // in the second window's edge guard: ignored
    { s: at + 300, i: 5, p: 0.8 },          // the same character, 31 ms later and more confident
  ]);
  assert.equal(out.filter((t) => t.i === 5).length, 1);
  assert.equal(out.find((t) => t.i === 5).p, 0.8);
  assert.ok(!out.some((t) => t.i === 9), 'edge-guard token ignored');
  // the first window's token past its reliable end (11.5 s > 11 s) is not committed either
  assert.ok(!out.some((t) => t.i === 7));
  assert.equal(m.committedUntil, CW_WIN - CW_EDGE_GUARD);
  // a different character at the same place: the more confident one wins
  const m2 = new CwMerger(CW_EDGE_GUARD, CW_TOLERANCE);
  m2.push(0, CW_WIN, [{ s: at, i: 5, p: 0.95 }]);
  const out2 = m2.push(hop, CW_WIN, [{ s: at - 200, i: 6, p: 0.5 }]);
  same(out2.map((t) => [t.i, t.p]), [[5, 0.95]]);
  // two copies further apart than the tolerance are not paired: each is kept by its half of the zone
  const m3 = new CwMerger(CW_EDGE_GUARD, CW_TOLERANCE);
  m3.push(0, CW_WIN, [{ s: 6 * SR, i: 5, p: 0.9 }]);
  const out3 = m3.push(hop, CW_WIN, [{ s: 6 * SR + CW_TOLERANCE + 1, i: 5, p: 0.9 }]);
  const middle = 0.5 * (hop + CW_EDGE_GUARD + CW_WIN - CW_EDGE_GUARD);
  assert.equal(out3.length, (6 * SR < middle ? 1 : 0) + (6 * SR + CW_TOLERANCE + 1 >= middle ? 1 : 0));
}

// 4. cwTokens on a hand-written log-probability matrix: run middles * 288, classes, confidence (softmax,
// the run's highest), blanks dropped, the word space kept
{
  const C = CW_VOCAB.length + 1;   // + blank
  assert.equal(CW_BLANK, C - 1);
  const E = CW_VOCAB.indexOf('E'), T_ = CW_VOCAB.indexOf('T');
  const frames = [CW_BLANK, E, E, E, CW_BLANK, CW_SPACE, CW_SPACE, T_, CW_BLANK];
  const peaks = [0.9, 0.5, 0.8, 0.7, 0.9, 0.6, 0.65, 0.95, 0.9];
  const data = new Float32Array(frames.length * C);
  frames.forEach((k, t) => {
    const rest = (1 - peaks[t]) / (C - 1);
    for (let c = 0; c < C; c++) data[t * C + c] = Math.log(c === k ? peaks[t] : rest);
  });
  const tok = cwTokens(data, [1, frames.length, C]);
  same(tok.map((t) => [t.s, t.i]), [[2 * 288, E], [Math.round(5.5 * 288), CW_SPACE], [7 * 288, T_]]);
  tok.forEach((t, n) => assert.ok(Math.abs(t.p - [0.8, 0.65, 0.95][n]) < 1e-6, 'confidence ' + n));
  assert.equal(cwTokText(tok), 'E T');
  assert.equal(CW_VOCAB[CW_SPACE], ' ');
  // the same on logits (not normalized): softmax gives the same probabilities
  const logits = data.map((v, n) => v + 3 * Math.floor(n / C));
  same(cwTokens(logits, [1, frames.length, C]).map((t) => [t.s, t.i]), tok.map((t) => [t.s, t.i]));
}

console.log('CW overlapping windows: step, merger, latency choices and CTC tokens passed (Rapid ' +
  results.fast.median.toFixed(1) + ' s, Normal ' + results.normal.median.toFixed(1) + ' s, Precise ' +
  results.precise.median.toFixed(1) + ' s median confirmation)');
