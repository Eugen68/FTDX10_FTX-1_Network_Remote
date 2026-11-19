# Future Improvement Ideas & Technical Notes

This document collects technical evaluations, architectural notes, and ideas for future improvements to the FTX-1 Network Companion.

---

## 1. On-Device FT8 Decoding on ESP32-S3

### Background & Goals
In the current version, the received USB audio from the FTX-1 is streamed over Wi-Fi directly to the browser, which performs full-stack FT8 decoding inside a Web Worker.
An alternative architectural option is to **decode FT8 in real-time directly inside the ESP32-S3 firmware and push only the decoded messages (text/JSON) to clients**.

- **Benefits**:
  - **Drastically Reduced Client Load**: Eliminates intensive DSP and LDPC decoding on the client device, allowing lightweight viewing on smartphones, tablets, and low-power devices.
  - **Independence from Audio Streaming**: Enables 24/7 background FT8 decoding and logging on the ESP32 without requiring an active audio stream.
  - **Bandwidth Reduction**: Avoids streaming raw PCM audio (around 768 kbps) when the user only wants FT8 spot monitoring.

### Memory (RAM) Feasibility Assessment
The ESP32-S3 has 512 KB of internal SRAM (typically ~300 to 380 KB of free user heap). This project targets standard DevKit boards without external PSRAM; on-device decoding is fully achievable within internal SRAM.

- **Streaming STFT (Short-Time Fourier Transform) & Waterfall Compression**:
  - Keeping 15 seconds of raw waveform (6.4 kHz / 16-bit mono) requires ~192 KB of RAM.
  - Instead, the audio is processed via streaming FFT as it arrives, compressing it into a **waterfall power matrix (`uint8_t` in dB)** as demonstrated in embedded libraries like [`ft8_lib`](https://github.com/kgoba/ft8_lib).
  - **Memory Estimation**:
    - Search bandwidth: 200 Hz to 2600 Hz (6.25 Hz/bin $\rightarrow$ 384 bins)
    - 15-second slot: 1 symbol = 0.16s $\rightarrow$ ~90 time blocks
    - Spectrogram storage: $384 \text{ bins} \times 90 \text{ blocks} \times 1 \text{ byte} \approx \mathbf{34.5 \text{ KB}}$ (or ~69 KB with $2\times$ time oversampling)
- **LDPC (174, 91) Decoding Work Memory**:
  - LLR arrays and BP (Belief Propagation) workspace require **a few KB to ~10 KB**.
- **Total Memory**: **~50 to 80 KB**, which comfortably fits alongside the Wi-Fi stack and HTTP server.

### CPU & Task Allocation
- **Dual-Core 240 MHz + ESP-DSP**:
  - **Core 0**: Wi-Fi, Web Server / WebSocket, USB Host (CAT & Audio reception)
  - **Core 1**: Dedicated FT8 decoding task (incremental FFT and end-of-slot candidate search / LDPC)
- Dedicated execution on Core 1 guarantees that FT8 decoding completes in ~0.3 to 0.8 seconds without dropping audio packets or stalling CAT polling.

### Delivery Architecture
- A WebSocket or Server-Sent Events (SSE) endpoint (e.g., `/api/ft8/live`) pushing JSON packets with UTC, SNR, DT, frequency, and decoded message text.

---

## 2. Other Potential Improvements

### A. Dual-Channel (MAIN / SUB) Decoding with PSRAM
- When using ESP32-S3 modules equipped with PSRAM (2 MB to 8 MB), simultaneously run parallel FT8 decoders for both MAIN (VFO-A) and SUB (VFO-B) without SRAM constraints.

### B. Two-Pass FT8 Decoding (Subtraction & Re-decoding)
- Implement a second decoding pass by subtracting reconstructed signals of successfully decoded stations from the waterfall to uncover weak stations underneath.

### C. Cloud / Network Spot Reporting (PSKReporter & MQTT)
- Direct autonomous reporting from the ESP32-S3 to [PSKReporter](https://pskreporter.info/) without a computer running.
- MQTT / Home Assistant integration for spot alerts and metrics.

### D. Mobile-Optimized Lightweight UI
- Responsive layout or dedicated lightweight dashboard optimized for smartphone screens.
