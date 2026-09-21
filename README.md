# gba4esp32

A standalone GBA / GB / GBC handheld firmware for a custom ESP32-S3 **N16R8**
(16 MB flash + 8 MB octal PSRAM) with a **320x240 ILI9341 SPI display**.

Derived from the original project:
https://github.com/Charlanth/esp32-gba

The current repository is a hardware-specific rebuild for one working handheld,
with the firmware organized under `components/esp_gba/`.

## Features

- Game Boy Advance emulation using the VBA-next / 44vba code lineage.
- Game Boy and Game Boy Color emulation using gnuboy.
- 240x160 native GBA image centered on the 320x240 ILI9341.
- SD-based game library, box art, sparse ROM packing, and persistent saves.
- GB/GBC speed options: 1.0x, 1.5x, 2.0x, 2.5x.
- GB/GBC Save State 1/2, Load State 1/2, Save Game, Reset, Exit to Menu, Resume.
- `Select + Start`: opens GB/GBC settings; on GBA, holding it for about 1.5 s returns to the main menu.
- Native USB-Serial-JTAG support for development and testing.
- Hardware-specific performance work, including threaded rendering and per-cartridge optimizations.

## Hardware

| Item | Current hardware |
|---|---|
| MCU | ESP32-S3 N16R8 |
| Flash | 16 MB |
| PSRAM | 8 MB octal |
| Display | ILI9341, 320x240 SPI |
| SD | SPI, shared with LCD |
| Audio | Physical audio disabled in this build |
| Buttons | Individual GPIO inputs |

Current LCD / SD pins:

| Signal | GPIO |
|---|---:|
| LCD CS | 10 |
| LCD DC | 11 |
| SPI SCK | 12 |
| SPI MOSI | 13 |
| SPI MISO | 9 |
| LCD Backlight | 14 |
| SD CS | 18 |

Button GPIOs are defined in `components/esp_gba/config.h`.

## ROMs and SD card

Put games in the matching folders:

```text
/roms/gba/*.gba
/roms/gb/*.gb
/roms/gbc/*.gbc
```

Other SD data:

```text
/art/       box art
/saves/     game saves and GB/GBC SRAM
/packed/    sparse ROM cache
```

`/packed/` may be deleted; it is rebuilt when needed.

## Audio

Physical audio output is deliberately disabled with:

```cpp
#define MY_RETROGO_NO_AUDIO 1
```

in `components/esp_gba/main.cpp`.

The emulator sound-processing code remains because it is part of the emulation
path, but the current build does not initialize physical audio hardware.

To restore physical audio, use the original project's ES8311/I2S implementation
as the reference:
https://github.com/Charlanth/esp32-gba

Restore a compatible codec/amplifier path and its GPIO configuration, then
change/remove the `MY_RETROGO_NO_AUDIO` guard. Removing the define alone is not
enough; the hardware wiring must match the audio code.

## Controls

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
| GBA L | 1 |
| GBA R | 21 |

Buttons are active-low with internal pull-ups.

### GB/GBC settings

`Select + Start` opens:

```text
GAME SETTINGS
Speed
Save State 1
Load State 1
Save State 2
Load State 2
Save Game
Reset Game
Exit to Main Menu
Resume
```

## What changed from the original project

This is a focused standalone handheld rebuild. The old `FNK0104AB`/generic
board-selection code, old button-matrix implementation, SDL2 port, libretro
frontend trees, duplicate old source layout, obsolete board build files,
development videos, old test/reference archives, and `flash_qio.sh` were removed
or relocated. The GBA core was moved to `components/gba/`, GB/GBC support is kept
under `components/gnuboy/`, and the handheld code is now under
`components/esp_gba/`.

Original/upstream projects:

- Original ESP32-GBA port: https://github.com/Charlanth/esp32-gba
- 44vba: https://github.com/44670/44vba
- VBA-next: https://github.com/libretro/vba-next
- gnuboy: https://github.com/rofl0r/gnuboy
- retro-go: https://github.com/ducalex/retro-go

## Tools

| Tool | Role |
|---|---|
| `tools/flash_qio.py` | Build and flash the firmware with the required DIO-patched bootloader. |
| `tools/patch_qio.py` | Convert the bootloader image from QIO to DIO before flashing. |
| `tools/pack_rom.py` | Build the sparse `.pak` and `.map` ROM cache on a PC. |
| `tools/trim_rom.py` | Remove trailing ROM padding before packing; interior holes are kept for the packer. |
| `tools/scrape_art.py` | Fetch/convert box art into the RGB565 `.raw` format used by the menu. |
| `tools/bench.py` | Run repeatable performance measurements. |
| `tools/bench_pick.py` | Pick a ROM over USB and measure emulator/drawn performance. |
| `tools/play.py` | Send scripted input and request framebuffer screenshots. |
| `tools/record.py` | Development framebuffer recording helper. |
| `tools/capture_boot.py` | Capture boot-time serial output for diagnostics. |
| `tools/aot/hotpc_report.py` | Turn hot-PC profiler output into an execution hotspot report. |
| `tools/aot/xlate.py` | Experimental static ARM/Thumb block translator. |
| `tools/aot/mixer_full.txt` | Reference/disassembly notes for the GBA m4a mixer. |
| `tools/aot/mixer_notes.md` | Notes from the native mixer investigation. |

## Build

Install PlatformIO, then from the repository root:

```bash
pio run -e esp32s3
```

### Clean build

Windows PowerShell:

```powershell
Remove-Item -Recurse -Force .pio
```

Linux/macOS:

```bash
rm -rf .pio
```

Then build again with `pio run -e esp32s3`.

## Flash

Use the QIO helper; do not use `pio run -t upload`.

Windows:

```powershell
python tools\flash_qio.py --port COM5
```

Linux/macOS:

```bash
python tools/flash_qio.py --port /dev/ttyACM0
```

The port can be omitted when auto-detection works:

```bash
python tools/flash_qio.py
```

## Fresh settings

To erase the NVS settings region, then flash again:

Windows:

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM5 erase_region 0x9000 0x6000
```

Linux/macOS:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_region 0x9000 0x6000
```

## GitHub

The GitHub repository is:

https://github.com/pcgamer404/gba4esp32

The first full local history was published to the new repository with:

```bash
git push -u origin main --force-with-lease
```

For normal future updates:

```bash
git add -A
git commit -m "Describe the change"
git push
```

## Development notes

Historical experiments, benchmarks, failed approaches, and engineering decisions
are kept in `docs/devlog/`.

For the current pinout, see `WIRING.md` and
`components/esp_gba/config.h`.

## Credits

This project is based on the work of:

- Charlanth/esp32-gba
- 44vba
- libretro/vba-next
- rofl0r/gnuboy
- ducalex/retro-go

See the linked repositories above for their original code and licenses.
