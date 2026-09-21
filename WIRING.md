# Hardware reference â€” esp-gba handheld

This document describes the fixed hardware pinout used by the current
ESP32-S3 handheld firmware.

The firmware is built specifically for this hardware. The pin assignments
below must match `components/esp_gba/config.h`.

---

## Important

The ESP32-S3 uses 3.3 V GPIO logic.

All GPIOs used by the firmware are active-low button inputs with the ESP32-S3
internal pull-up enabled.

Do not reuse GPIOs connected internally to the ESP32-S3 module's flash or PSRAM.

---

## 1. LCD â€” ILI9341 320Ã—240 SPI

The display is an ILI9341 panel running in landscape orientation.

| LCD signal | ESP32-S3 GPIO | Notes |
|---|---:|---|
| `CS` | 10 | LCD chip select |
| `DC` | 11 | Data / command |
| `SCK` | 12 | SPI clock |
| `MOSI` / `SDI` | 13 | SPI data |
| `MISO` / `SDO` | Not connected | LCD readback is disabled |
| `RESET` | Not connected | LCD reset is tied to the ESP32-S3 reset line |
| `LED` / backlight | 14 | Firmware-controlled backlight |
| `VCC` | 3.3 V | |
| `GND` | GND | |

The LCD uses the same SPI clock, MOSI and MISO lines as the SD card.

The firmware drives the ILI9341 at up to 40 MHz.

The emulator's native GBA framebuffer is 240Ã—160 and is centered on the
320Ã—240 display.

---

## 2. SD card â€” SPI

The SD card shares the SPI bus with the LCD.

| SD signal | ESP32-S3 GPIO |
|---|---:|
| `CS` | 18 |
| `MOSI` | 13 |
| `MISO` | 9 |
| `CLK` | 12 |
| `VCC` | 3.3 V |
| `GND` | GND |

LCD and SD card therefore share:

- `GPIO 12` â€” SPI clock
- `GPIO 13` â€” SPI MOSI
- `GPIO 9` â€” SPI MISO

Each device has its own chip-select GPIO.

---

## 3. Buttons

Buttons are connected between the GPIO and GND.

The firmware enables the ESP32-S3 internal pull-ups, so:

- released = HIGH
- pressed = LOW

| Button | GPIO |
|---|---:|
| Up | 8 |
| Down | 6 |
| Left | 7 |
| Right | 15 |
| A | 47 |
| B | 40 |
| Select | 5 |
| Start | 2 |

Two additional buttons are used as Game Boy Advance shoulder buttons:

| GBA button | GPIO |
|---|---:|
| L | 1 |
| R | 21 |

The same physical button mapping is used by the launcher/menu and the emulator.

---

## 4. Button mapping summary

| Function | GPIO |
|---|---:|
| Up | 8 |
| Down | 6 |
| Left | 7 |
| Right | 15 |
| A | 47 |
| B | 40 |
| Select | 5 |
| Start | 2 |
| GBA L | 1 |
| GBA R | 21 |

---

## 5. Audio

Physical audio output is currently disabled in the firmware.

The emulator's sound-processing code remains present because parts of the
emulation stack use it for timing, but this handheld build does not
initialize a physical audio output device.

There is therefore no active audio GPIO pin assignment in the current
firmware.

---

## 6. Battery

The current firmware does not use a battery-voltage ADC.

`osBatteryMv()` returns `-1` because this hardware configuration has no
battery-voltage measurement input defined in `config.h`.

Battery charging and power regulation are therefore outside the scope of the
firmware.

---

## 7. Touch controller

If the ILI9341 module includes an XPT2046 or similar touch controller, it is
not used by this firmware.

No touch GPIOs are configured.

---

## 8. GPIOs reserved by the ESP32-S3 module

Avoid using the ESP32-S3 module's internal flash/PSRAM GPIOs for external
hardware.

The firmware's active GPIO assignments are defined in:

`components/esp_gba/config.h`

Do not change those assignments unless the physical hardware is also changed.

---

## 9. Complete active pin map

| GPIO | Function |
|---:|---|
| 1 | GBA L |
| 2 | Start |
| 5 | Select |
| 6 | Down |
| 7 | Left |
| 8 | Up |
| 9 | SD MISO |
| 10 | LCD CS |
| 11 | LCD DC |
| 12 | Shared SPI SCK |
| 13 | Shared SPI MOSI |
| 14 | LCD backlight |
| 15 | Right |
| 18 | SD CS |
| 21 | GBA R |
| 40 | B |
| 47 | A |

All other GPIOs are outside the firmware's active handheld control/display
mapping.
