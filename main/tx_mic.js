/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
const voiceRate = 16000;
const frameSamples = voiceRate / 50;
const taps = 48;
const phases = 32;

class TxMicrophone extends AudioWorkletProcessor {
  constructor() {
    super();
    this.history = new Float32Array(taps);
    this.writeIndex = 0;
    this.untilSample = sampleRate;
    this.frame = new Int16Array(frameSamples);
    this.frameIndex = 0;
    this.peak = 0;
    this.gain = 1;
    this.port.onmessage = ({ data }) => {
      if (Number.isFinite(data.gain)) this.gain = data.gain;
    };

    // Delay the kernel so every output uses available samples. The phase
    // compensates for the fractional position of the 16 kHz output sample.
    const cutoff = 0.42 * Math.min(1, voiceRate / sampleRate);
    this.kernels = Array.from({ length: phases }, (_, phase) => {
      const center = (taps - 1) / 2 + phase / phases;
      const coeffs = new Float32Array(taps);
      let sum = 0;
      for (let k = 0; k < taps; ++k) {
        const t = 2 * Math.PI * cutoff * (k - center);
        const window = 0.5 - 0.5 * Math.cos(2 * Math.PI * k / (taps - 1));
        coeffs[k] = window * (t === 0 ? 1 : Math.sin(t) / t);
        sum += coeffs[k];
      }
      return coeffs.map(value => value / sum);
    });
  }

  process(inputs) {
    const input = inputs[0]?.[0];
    if (!input) return true;

    for (const sample of input) {
      this.history[this.writeIndex] = sample;
      this.writeIndex = (this.writeIndex + 1) % taps;
      this.untilSample -= voiceRate;
      while (this.untilSample <= 0) {
        const phase = Math.min(phases - 1, Math.floor(-this.untilSample * phases / voiceRate));
        const kernel = this.kernels[phase];
        let value = 0;
        let pos = this.writeIndex;
        for (const coeff of kernel) {
          pos = pos === 0 ? taps - 1 : pos - 1;
          value += coeff * this.history[pos];
        }
        value *= this.gain;
        this.peak = Math.max(this.peak, Math.abs(value));
        this.frame[this.frameIndex++] = Math.round(Math.max(-32768, Math.min(32767, value * 32768)));
        if (this.frameIndex === this.frame.length) {
          this.port.postMessage({pcm: this.frame, peak: this.peak}, [this.frame.buffer]);
          this.frame = new Int16Array(frameSamples);
          this.frameIndex = 0;
          this.peak = 0;
        }
        this.untilSample += sampleRate;
      }
    }
    return true;
  }
}

registerProcessor('tx-microphone', TxMicrophone);
