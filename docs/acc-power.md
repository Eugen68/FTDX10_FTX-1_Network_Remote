# Powering the board from the FTDX10 ACC socket (optional)

This note describes how to power the ESP32-S3 board from the FTDX10's own 13.8 V supply on the ACC socket,
so that the board turns on and off with the radio and no separate USB charger is needed.

It is a hardware proposal. It is **not tested yet** on the author's station. Read it all before you build it.

> **Check the FTDX10 operating manual first.** The README lists ACC pin 8 as 13.8 V. Confirm the pin and the
> **maximum current** the manual allows on it before you connect anything. Accessory outputs are usually
> current limited and may have an internal fuse. Never connect pin 8 to the board directly: always go through
> the fuse, the protection parts and the 5 V converter described below.
>
> Connect and disconnect the ACC plug only with the radio's power supply off.

## 1. Current budget

On its 5 V rail the board feeds:

- the ESP32-S3 with Wi-Fi: about 150–250 mA on average, with peaks of about 400–500 mA while Wi-Fi transmits;
- the radio's USB port (VBUS), when the board's USB-OTG power bridge is closed. Some of the radio's USB
  circuits (hub, serial bridges, audio codec) may draw from it.

So plan for **5 V at 1 A** with margin. Seen from the 13.8 V side:

| Regulator | Current drawn from ACC pin 8 (about 2.5 W at 5 V) |
|---|---|
| Switching module (about 90 % efficient) | **about 0.2 A** |
| Linear regulator (7805) | **about 0.4–0.5 A**, plus about 3.5 W of heat |

From the ACC socket a **switching module is the right choice**: it draws about half the current. A linear
regulator could exceed the socket's limit. If the manual allows less than about 300 mA, check that the board
stays below it, including the Wi-Fi peaks.

## 2. Converter

Use a ready-made, good-quality "7805 replacement" switching module, not a cheap adjustable buck module.
Any of these (5 V output):

| Model | Output | Input | Notes |
|---|---|---|---|
| Traco Power TSR 1-2450 | 5 V / 1 A | 6.5–36 V | about 94 % efficient, widely available |
| RECOM R-78E5.0-1.0 | 5 V / 1 A | 7–28 V | its datasheet gives the recommended EMC filter |
| Murata OKI-78SR-5/1.5-W36 | 5 V / 1.5 A | 7–36 V | more current margin |

They come in a 3-pin 7805 footprint and need no external inductor of their own. Check the input and output
capacitors and the EMC (class B) filter in the datasheet of the model you choose; the values below are in the
same range.

## 3. Circuit

```
ACC pin 8 (+13.8 V) ─[F1]─[D1]─┬───[L1]───┬── IN  ┌───────────┐ OUT ──┬──[L2]──┬──[FB]── board 5V pin
                               │          │       │  5 V      │       │        │
                            [TVS1]     [C1][C2]   │  module   │    [C3][C4]  [C5][C6]
                               │          │       └─────┬─────┘       │        │
ACC pin 2 (GND) ───────────────┴──────────┴─────────── GND ───────────┴────────┴────────── board GND
```

| Part | Value | Purpose |
|---|---|---|
| F1 | resettable fuse (PTC) 0.5 A, for example Bourns MF-R050 | current limit; choose it below the ACC pin limit from the manual |
| D1 | 1N5819 (Schottky, 1 A / 40 V) | reverse polarity protection (about 0.3 V drop) |
| TVS1 | SMBJ18A | clamps voltage spikes on the 13.8 V line |
| L1 | shielded inductor 10–22 µH, at least 1 A | keeps the converter's noise out of the radio's supply |
| C1, C2 | 22 µF / 35 V electrolytic and 100 nF ceramic | input filter, close to the module |
| C3, C4 | 10 µF ceramic and 100 nF | output filter, close to the module |
| L2 | shielded inductor 10 µH, at least 1 A | second filter stage on 5 V |
| C5, C6 | 100 µF low-ESR and 100 nF | reservoir for the Wi-Fi current peaks |
| FB | ferrite bead or clip-on ferrite, Fair-Rite mix 31 | HF noise on the wire to the board; put one on the USB cable to the radio as well |

- The 5 V output goes to the board's **5V pin**, never to 3V3. The board's own regulator makes 3.3 V, and the
  5 V rail also feeds the radio's USB port through the USB-OTG power bridge.
- Join the grounds at one point (star ground): ACC ground, the module's ground and the board's GND.

## 4. Layout and enclosure

- Put the **converter and its filters in a small metal box** (or under a metal shield) connected to ground.
- Keep the **board itself in a plastic case**: on most boards the Wi-Fi antenna is on the board, and a metal box
  around it blocks Wi-Fi (see "Recommended board" in the README). If the board must go in a metal box, use a
  board with an external antenna connector and put the antenna outside.
- Keep the wires short. Route the power wire and the USB cable away from the coaxial cable.

## 5. PC connection while powered from the radio

Many boards have a diode between the USB-UART port's VBUS and the 5 V rail, so a PC on that port is not fed from
the radio's supply. Check this on your board's schematic. Until you have checked it, do not connect a PC to the
USB-UART port while the board is powered from the ACC socket.

## 6. Noise test (required)

A switching converter can add noise to the receiver: a higher noise floor or fixed carriers ("birdies").
After building it:

1. Put the radio on receive with no antenna (or with a dummy load).
2. Go through all bands, especially 160–40 m, and compare the noise floor and the S-meter with the board
   **powered from the ACC socket** and **powered from USB** as before.
3. Look for fixed carriers that appear only with the converter.

If it is noisier, in this order: add more ferrites (both ends of the USB cable, the power wire), improve the LC
filter, shield the converter. The quietest fallback is a **linear 7805 with a heatsink**, but only if the manual
confirms that the ACC pin can supply at least 0.6 A.

## 7. What changes in operation

- The board starts and stops with the radio. It cannot be used to turn the radio on remotely (that is not
  possible over CAT anyway).
- The board and the radio start together. The firmware waits 3 s after the radio appears on USB before the first
  CAT byte, so start-up is handled the same way as when the radio is turned on with the board already running.
- This applies to the **FTDX10 only**. The FTX-1 does not have this ACC socket and needs a different supply.
