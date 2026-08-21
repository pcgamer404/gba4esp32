# Wiring guide — esp-gba handheld

Written for someone who has not done much wiring. Read "Pins you must not use" first;
it is the part that quietly ruins boards.

Everything here matches the pin numbers in `port-esp32s3/main/os.h`. Change one, change
the other.

---

## The one rule that matters

**The ESP32-S3 runs at 3.3 V logic and is not 5 V tolerant.** Putting 5 V on any GPIO can
permanently damage the chip. Modules that say "5 V" usually mean *their power input* is 5 V
while their data pins are still 3.3 V — but check each one before connecting.

Second rule: **everything shares a common GND.** Every module, the battery, the board. If
grounds are not tied together, nothing works and the symptoms look random.

---

## Pins you must not use

| Pins | Why |
|---|---|
| **26–37** | Wired to the flash chip and the octal PSRAM. Using them crashes the board. Non-negotiable on this module. |
| **0, 3, 45, 46** | Strapping pins — sampled at reset to pick boot mode. GPIO 0 low at power-on = USB download mode, so the console just won't boot. |
| **43, 44** | UART0 console (TX/RX). You need these for flashing and logs. |
| **19, 20** | Native USB D−/D+. |

Free and safe: **1, 2, 4–18, 21, 38–42, 47, 48**.

Upstream's button map used 0, 45 and 46. I remapped it — see the button table below.

---

## 1. Screen — ILI9341 2.4" 320×240 SPI

Most of these modules are the red "TFT_SPI 2.4" boards with 14 pins. They have a 3.3 V
regulator and often a level shifter, so `VCC` takes 5 V or 3.3 V — but **feed it 3.3 V** and
you sidestep the whole question.

| Display pin | ESP32-S3 | Notes |
|---|---|---|
| `VCC` | 3V3 | |
| `GND` | GND | |
| `CS` | GPIO 11 | chip select |
| `RESET` | GPIO 21 | |
| `DC` / `RS` | GPIO 10 | data/command |
| `SDI` / `MOSI` | GPIO 13 | |
| `SCK` | GPIO 12 | |
| `LED` | 3V3 (see note) | backlight |
| `SDO` / `MISO` | GPIO 14 | optional — only for reading back or touch |

**Backlight:** most modules already have a series resistor on `LED`; tie it to 3V3. If yours
does not, put **100 Ω** in series or you will cook the backlight LEDs. If you want brightness
control later, drive `LED` from GPIO 47 through a transistor — do not sink the whole backlight
current through a GPIO (40 mA absolute max per pin, and the backlight wants more).

**The touch controller** (`T_CLK`, `T_CS`, `T_DIN`, `T_DO`, `T_IRQ`) is a separate XPT2046 chip.
Leave it disconnected — nothing uses it.

⚠️ **The firmware does not drive an ILI9341 yet.** `os.c` has an ST7789 240×240 init sequence.
The ILI9341 needs a different init and different window maths. Wiring it up will produce a
blank or garbled screen until that is written. Ping me when the panel lands.

---

## 2. Buttons — 8 tactile switches

The easiest wiring in the whole project. Each button connects **its GPIO to GND**. Nothing
else — no resistors. The firmware enables the chip's internal pull-ups, so the pin idles high
and reads low when pressed.

| Button | GPIO |
|---|---|
| Up | 4 |
| Down | 5 |
| Left | 6 |
| Right | 7 |
| A | 15 |
| B | 16 |
| Select | 17 |
| Start | 18 |

A 12 mm tactile switch has 4 legs, but they are **two pairs already joined inside**. If the
button seems permanently pressed, you picked two legs from the same pair — rotate it 90°.

Run one wire from each switch to its GPIO, and daisy-chain the other side of all eight to a
single GND wire.

---

## 3. Speaker — MAX98357A I2S amplifier

**Important: the ESP32-S3 has no DAC.** The original ESP32 had analogue output on GPIO 25/26;
Espressif removed it on the S3. Tutorials that wire a speaker straight to a GPIO are written
for the old chip and will not work here. You need a digital (I2S) amplifier.

The **MAX98357A** is the right part — it is a DAC and a 3 W class-D amplifier in one, takes
I2S directly, and costs a few euros.

| MAX98357A | ESP32-S3 |
|---|---|
| `VIN` | 3V3 (or 5 V for more volume) |
| `GND` | GND |
| `BCLK` | GPIO 40 |
| `LRC` / `WS` | GPIO 41 |
| `DIN` | GPIO 42 |
| `GAIN` | leave floating (9 dB default) |
| `SD` | leave floating (enabled) |

Speaker wires to the `+` / `−` screw terminal. Use a **4 Ω or 8 Ω, 1–3 W** speaker.

Do **not** connect the speaker to GND — the output is bridged (both terminals swing). Tying
one side to ground damages the amp.

⚠️ **Audio is not implemented either.** `systemOnWriteDataToSoundBuffer()` is an empty stub in
upstream, so there is nothing to send yet. Wire it if you like, but expect silence.

---

## 4. Battery — the part that can actually hurt you

Lithium cells store real energy. A shorted or reverse-connected LiPo can vent flame. None of
this is exotic to get right, but it is worth getting right.

**Recommended, simplest safe option: a combined charger + boost module** (sold as "5 V 1 A
power bank module" or "TP4056 + boost"). One board handles charging, protection and stepping
up to 5 V.

```
LiPo cell ──> [TP4056 + protection + 5V boost] ──> ESP32-S3  5V pin
                          │                                  GND pin
                          └── USB-C in for charging
```

Feed the module's 5 V output into the board's **5V / VIN pin**, not 3V3 — the board's own
regulator makes the 3.3 V. Feeding 5 V into a 3V3 pin destroys the board.

**Buy the TP4056 board that has protection** — it has two extra chips near the battery
terminals (DW01A + a dual FET) and pads labelled `B+ B− OUT+ OUT−`. The bare version without
those has no over-discharge or short protection. If your module only has `B+ B−` and `OUT+
OUT−` is missing, it is the unprotected kind.

Rules:

- **Check polarity three times before the first connection.** Red = `B+`, black = `B−`.
  Reversing it is the one mistake with no recovery.
- Use a cell with a **JST-PH 2.0 connector** already fitted rather than soldering to bare
  pouch tabs. Soldering directly to a cell risks internal shorts from heat.
- **1000–2000 mAh** is plenty. Rough draw here is 100–150 mA (no Wi-Fi), plus up to ~100 mA
  for the backlight, so ~2000 mAh gives several hours.
- Do not charge it unattended the first few times. Charge on something non-flammable.
- Never puncture, crush, or keep charging a cell that has swollen. A puffy cell is done —
  dispose of it at a battery collection point.

Powering from USB and battery at the same time is fine with the protected TP4056 modules; they
handle the changeover.

---

## Suggested build order

Do these one at a time and test between each. If you wire everything and it doesn't work, you
have eight suspects instead of one.

1. **Buttons first** — cheapest, safest, and testable today. The firmware already reads them,
   and `tools/play.py` lets me compare against injected input.
2. **Battery** — before the screen, so you're not chasing brownouts later.
3. **Screen** — after I've written the ILI9341 driver.
4. **Speaker** — last, since the audio path doesn't exist yet.

## Pin map summary

| GPIO | Use |
|---|---|
| 4, 5, 6, 7 | Up, Down, Left, Right |
| 10, 11, 12, 13, 14, 21 | LCD DC, CS, SCK, MOSI, MISO, RESET |
| 15, 16, 17, 18 | A, B, Select, Start |
| 40, 41, 42 | I2S BCLK, LRC, DIN |
| 47 | spare — backlight PWM |
| 1, 2, 8, 9, 38, 39, 48 | free (SD card would go here) |
