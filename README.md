# esp-gba â€” custom ESP32-S3 GBA / GB / GBC handheld

A stripped and rebuilt ESP32-S3 handheld firmware derived from the original
[Charlanth/esp32-gba](https://github.com/Charlanth/esp32-gba) project.

This repository targets a **fixed custom handheld configuration** rather than a
generic multi-board port.

> **Hardware target:** ESP32-S3 **N16R8** â€” **16 MB flash + 8 MB octal PSRAM** â€”
> with a **320Ã—240 ILI9341 SPI display**, microSD storage, and the physical
> buttons defined in `components/esp_gba/config.h`.

The hardware is already assembled and working. This repository is a software
cleanup and adaptation of the original project: obsolete board targets,
desktop ports, duplicate source trees, unused libraries, stale documentation,
and development-only artifacts have been removed or relocated.

---

## What this project is

The original project was an experimental ESP32-S3 GBA port based around VBA-next
and the Freenove ESP32-S3 display hardware.

This repository keeps the useful emulator and ESP32 work, but turns it into a
single-purpose handheld firmware for the current hardware.

The main upstream/original project is:

- **Original project:** [Charlanth/esp32-gba](https://github.com/Charlanth/esp32-gba)
- **GBA core lineage:** [44670/44vba](https://github.com/44670/44vba) â†’ [libretro/vba-next](https://github.com/libretro/vba-next)
- **GB/GBC core:** [rofl0r/gnuboy](https://github.com/rofl0r/gnuboy)
- **GB/GBC reference/vendor source:** [ducalex/retro-go](https://github.com/ducalex/retro-go)

The current repository should therefore be viewed as a **hardware-specific
derivative**, not as a clean upstream fork with every original target intact.

---

# Features

## Game Boy Advance

- VBA-next based GBA emulation.
- 240Ã—160 native GBA framebuffer centered on the 320Ã—240 ILI9341.
- SD-based ROM library and boot-time ROM picker.
- Sparse ROM packing so large padded cartridges can fit the available ROM
  partition when their distinct 64 KB pages fit.
- Per-cartridge discovery for the GBA optimizations that were added during
  development.
- Persistent GBA save support through the SD card.
- Native USB-Serial-JTAG control path for development tools and scripted
  testing.
- Optimized threaded rendering and other measured performance work retained
  from the development process.

## Game Boy / Game Boy Color

- GB/GBC emulation through vendored **gnuboy**.
- Library routing by `.gb` / `.gbc` extension.
- Full-screen 320Ã—240 presentation using the GB/GBC scaler.
- Locked 60 fps on the supported GB/GBC path.
- SRAM save support.
- In-game settings menu.
- Selectable emulation speed: **1.0Ã— / 1.5Ã— / 2.0Ã— / 2.5Ã—**.
- **Save State 1 / Load State 1** and **Save State 2 / Load State 2**.
- Save Game, Reset Game, Exit to Main Menu, and Resume options.

---

# Controls

The current firmware uses **individual active-low buttons**, not the old
Freenove 2Ã—4 matrix.

The exact GPIO assignments are kept in:

```text
components/esp_gba/config.h
```

Current map:

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

Buttons use the ESP32-S3 internal pull-ups.

## Select + Start

`Select + Start` has a deliberate different role depending on the running
system:

- **GBA:** hold `Select + Start` for about 1.5 seconds to leave the game and
  return to the main menu.
- **GB/GBC:** `Select + Start` opens the **Game Settings** screen.

The GB/GBC settings screen contains:

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

For GB/GBC, `B` resumes/backs out, while the directional buttons navigate the
settings and `A` selects/toggles.

---

# Hardware

## ESP32-S3

- ESP32-S3
- 16 MB flash
- 8 MB octal PSRAM
- Native USB-Serial-JTAG
- Hardware configuration is fixed in `components/esp_gba/config.h`

## Display

- ILI9341
- 320Ã—240
- SPI
- 40 MHz configured in the current hardware port
- Display reset is tied to the ESP32-S3 reset line
- LCD readback is disabled in the normal firmware path
- The GBA game image remains 240Ã—160 and is centered on the LCD

Current LCD wiring:

| Signal | GPIO |
|---|---:|
| CS | 10 |
| DC | 11 |
| SCK | 12 |
| MOSI | 13 |
| MISO | 9 |
| Backlight | 14 |

## SD card

The SD card shares the SPI bus with the LCD:

| Signal | GPIO |
|---|---:|
| CS | 18 |
| SCK | 12 |
| MOSI | 13 |
| MISO | 9 |

## Audio

### Current build: physical audio disabled

The current handheld build deliberately disables **physical audio output**.

In `components/esp_gba/main.cpp` the build contains:

```cpp
#define MY_RETROGO_NO_AUDIO 1
```

The surrounding code checks this define before initializing the physical audio
path.

The reason is deliberate: this repository was cleaned around the current
handheld hardware configuration, while the original project contains a
Freenove-specific ES8311/SC8002B audio path. Rather than leaving a misleading
or board-specific audio initialization path enabled, the current build keeps
the emulator sound-processing code where the core requires it, but does not
initialize a physical output device.

This is a **software disable**, not removal of the original audio work.

### Re-enabling audio from the original project

The original implementation can be used as the reference for restoring audio:

[Charlanth/esp32-gba](https://github.com/Charlanth/esp32-gba)

The original tree contains the ES8311/I2S implementation and the associated
Freenove hardware configuration.

The current software gate is:

```cpp
#define MY_RETROGO_NO_AUDIO 1
```

in:

```text
components/esp_gba/main.cpp
```

To bring physical audio back, the ES8311/I2S initialization path and its GPIO
configuration from the original project must be restored and the
`MY_RETROGO_NO_AUDIO` guard changed/removed accordingly.

**Important:** removing the define alone does not create an audio device. The
audio code must match the actual hardware wiring. The original audio path is
specific to the hardware used by the original project, so only re-enable it
when the target hardware has a compatible codec/amplifier and matching I2S
connections.

---

# What was removed from the original repository

This repository intentionally does **not** keep every directory from the
original project.

The goal was to remove code that is unrelated to the standalone handheld
firmware, reduce ambiguity, and make the repository easier to build and
maintain.

## Removed or stripped

### `libretro/`
Removed because the standalone handheld build does not use the libretro
frontend/library tree.

### `libretro-common/`
Removed together with the unused libretro frontend support.

### `port-sdl2/`
Removed because this repository is no longer maintaining a desktop SDL port.
The handheld firmware is the product target.

### Root `src/`
Removed as a duplicate/old project layout after the GBA core was moved into:

```text
components/gba/
```

### `video/`
Removed because it belonged to the old project layout and was not part of the
current standalone ESP32-S3 firmware.

### `shots/`
Removed from the old location; screenshots are kept under:

```text
docs/screenshots/
```

### Old test/development documents
Obsolete vendored test/document artifacts such as old CPU reference PDFs and
unused test archives were removed to keep the repository focused on firmware.

### Old board-specific build files
Removed obsolete files tied to the previous board arrangement, including the
old port-local CMake/sdkconfig/partition and Windows flash helper files.

### `tools/flash_qio.sh`
Removed because the repository now uses the cross-platform Python helper:

```text
tools/flash_qio.py
```

### Old board-selection code
The previous `FNK0104AB`/generic board selection path was removed.

The current firmware is a **single fixed hardware target**. There is no
`ESP_GBA_BOARD=...` selection anymore.

### Old button-matrix firmware
The old Freenove 2Ã—4 GPIO matrix implementation was removed.

The current firmware reads the actual individual buttons directly from the GPIO
assignments in `config.h`.

---

# What was kept

The cleanup did not remove code just because it came from upstream.

Important files and components remain because the firmware depends on them.

## `components/gba/`

The GBA core and its required support files now live in a normal ESP-IDF
component.

## `components/gnuboy/`

The GB/GBC emulator is kept as the second emulation engine.

## `components/esp_gba/`

This is the handheld port:

- display
- SD card
- buttons
- menu
- ROM loading/packing
- save handling
- serial control
- timing/performance work

---

# Tools

Every script in `tools/` has a specific development or maintenance purpose.

| Tool | Role |
|---|---|
| `flash_qio.py` | Builds the firmware and flashes the QIO app with a DIO-patched bootloader. |
| `flash_qio.py` / `patch_qio.py` | The QIO helper uses `patch_qio.py` to convert the bootloader image back to DIO before flashing. |
| `bench.py` | Runs the benchmark harness and reports emulator/drawn performance. |
| `bench_pick.py` | Selects a ROM over the serial control channel, waits for boot, and reports emu/draw performance. |
| `play.py` | Sends scripted key input and requests framebuffer screenshots over the development serial channel. |
| `record.py` | Development recording/capture helper for the serial framebuffer path. |
| `capture_boot.py` | Captures/parses boot-time serial output for development diagnostics. |
| `pack_rom.py` | Creates the sparse `.pak` + `.map` ROM cache on a PC before copying it to the SD card. |
| `trim_rom.py` | Removes trailing ROM padding from a cartridge image; it does not remove interior holes. |
| `patch_qio.py` | Patches a bootloader image from QIO back to DIO for the ESP32-S3 ROM-loader constraint. |
| `scrape_art.py` | Fetches/converts game box art into the small RGB565 `.raw` files used by the menu. |
| `bench_pick.py` | Automates ROM selection and performance measurement through the firmware command channel. |
| `bench.py` | General benchmark runner used for repeated performance measurements. |
| `hotpc_report.py` | Converts the hot-PC profiler output into a report for identifying execution hotspots. |
| `tools/aot/xlate.py` | Development translator used for experimental static ARM/Thumb block translation. |
| `tools/aot/mixer_full.txt` | Disassembly/reference notes for the GBA m4a mixer investigation. |
| `tools/aot/mixer_notes.md` | Notes describing the native mixer reverse-engineering work. |

Some tools are development aids rather than tools required for normal users.

---

# SD card layout

Create/use the following directories on the microSD card:

```text
/roms/
/art/
/saves/
/packed/
```

## ROMs

Put:

```text
.gba
.gb
.gbc
```

files in:

```text
/roms/
```

## Box art

Optional box art goes in:

```text
/art/
```

using the ROM filename without the extension and the format generated by
`scrape_art.py`.

## Saves

The firmware creates/uses:

```text
/saves/
```

for game saves and GB/GBC SRAM.

Examples:

```text
Pokemon Emerald.sav
Pokemon Crystal.srm
```

## Packed ROM cache

The sparse ROM cache uses:

```text
/packed/
```

with `.pak` and `.map` files.

The cache can be deleted safely; it will be rebuilt when required.

---

# Build environment

The project uses:

- PlatformIO
- ESP-IDF through PlatformIO
- Espressif32 platform `5.4.0`
- ESP32-S3 environment named `esp32s3`

The active build configuration is in:

```text
platformio.ini
```

The hardware definition is in:

```text
components/esp_gba/config.h
```

---

# Build

## Normal build

From the repository root:

```bash
pio run
```

or:

```bash
pio run -e esp32s3
```

The project uses the `esp32s3` environment as the handheld target.

## Windows

PowerShell:

```powershell
pio run -e esp32s3
```

## Linux

```bash
pio run -e esp32s3
```

---

# Clean build

## Normal PlatformIO clean

```bash
pio run -t clean
```

## Full build-directory removal

Windows PowerShell:

```powershell
Remove-Item -Recurse -Force .pio
```

Linux/macOS:

```bash
rm -rf .pio
```

A full `.pio` removal is useful after changing CMake environment variables,
board definitions, or other settings that require a clean reconfiguration.

---

# Headless build

The project has an optional headless mode for emulator/core testing.

It skips LCD initialization and GPIO setup.

## Windows PowerShell

```powershell
$env:ESP_GBA_HEADLESS="1"
pio run -e esp32s3
Remove-Item Env:ESP_GBA_HEADLESS
```

## Linux/macOS

```bash
ESP_GBA_HEADLESS=1 pio run -e esp32s3
```

Headless mode is intended for development and testing, not normal handheld
operation.

---

# Flashing

## Important: use `flash_qio.py`

The ESP32-S3 flash setup requires the application to remain QIO while the
bootloader image is patched back to DIO.

Use:

```text
tools/flash_qio.py
```

Do **not** use:

```bash
pio run -t upload
```

The helper performs:

1. firmware build
2. bootloader QIO â†’ DIO patching
3. partition image selection
4. firmware/bootloader flashing with esptool

## Windows

Example:

```powershell
python tools\flash_qio.py --port COM5
```

Replace `COM5` with the actual serial port.

## Linux

Example:

```bash
python tools/flash_qio.py --port /dev/ttyACM0
```

The port can be omitted when automatic detection works:

```bash
python tools/flash_qio.py
```

## Flash from a completely clean build

Windows:

```powershell
Remove-Item -Recurse -Force .pio
pio run -e esp32s3
python tools\flash_qio.py --port COM5
```

Linux/macOS:

```bash
rm -rf .pio
pio run -e esp32s3
python tools/flash_qio.py --port /dev/ttyACM0
```

The flash helper itself also performs the build, so the most common command is
simply:

```bash
python tools/flash_qio.py --port COM5
```

---

# Factory/fresh-settings reset

The firmware stores user settings in NVS.

To clear the NVS region, use esptool through PlatformIO.

## Linux/macOS

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_region 0x9000 0x6000
```

## Windows PowerShell

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM5 erase_region 0x9000 0x6000
```

Then flash the firmware again with `tools/flash_qio.py`.

Use this when you need a clean settings state rather than simply rebuilding the
firmware.

---

# Serial development tools

The firmware uses the ESP32-S3 native USB-Serial-JTAG path.

The control protocol is defined in:

```text
components/esp_gba/os.h
```

Development tools such as:

```text
bench.py
bench_pick.py
play.py
record.py
capture_boot.py
```

use that interface for automated testing, scripted input, framebuffer capture,
and performance measurements.

The normal handheld does not require these tools.

---

# Repository layout

```text
components/
  gba/                  GBA emulator/core
  gnuboy/               GB/GBC emulator

components/esp_gba/
  main/                 Handheld application and hardware port

tools/
  flash_qio.py          Build + flash helper
  pack_rom.py           Sparse ROM cache creation
  trim_rom.py           Trailing ROM padding removal
  scrape_art.py         Box-art conversion/fetching
  bench*.py             Performance tools
  play.py               Input/screenshot tool
  aot/                  Experimental translation tooling

docs/
  screenshots/          Current project screenshots
  devlog/               Historical engineering notes
  photos/               Build photographs

hardware/
  README.md             Hardware documentation
  case/                 Case information
  pcb/                  PCB information
```

---

# Development history and major changes

This repository contains a substantial amount of work beyond the original
ESP32-GBA starting point.

Among the major changes are:

- fixed the project around the current ESP32-S3 handheld hardware
- replaced old generic/Freenove board-selection logic with one real `config.h`
- moved the GBA core into `components/gba/`
- added and integrated GB/GBC support through gnuboy
- added the SD-based ROM library and menu
- added sparse ROM packing/caching for larger GBA cartridges
- added persistent save handling
- added box-art support
- added native USB-Serial-JTAG control
- added scripted ROM selection and benchmark tooling
- added GBA/GB/GBC settings handling
- added GB/GBC speed selection
- added GB/GBC save states
- added `Select + Start` context-sensitive controls
- added measured renderer and emulator optimizations
- added headless build support
- removed obsolete desktop/libretro code and old board-specific code

The detailed engineering history is retained under:

```text
docs/devlog/
```

Those files contain historical experiments, measurements, failures, and fixes.
They should not be treated as the current hardware wiring reference.

---

# Current hardware reference

For the authoritative current pinout, use:

```text
components/esp_gba/config.h
WIRING.md
```

The firmware is intentionally tied to that hardware configuration.

Do not copy the old FNK0104 matrix wiring from historical documentation into
a new build; that support was removed as part of the cleanup.

---

# Performance notes

Performance is title/scene dependent.

The repository contains a large amount of measured optimization work in:

```text
docs/devlog/FINDINGS.md
docs/devlog/SESSION.md
```

The key point for users is that GBA speed is not identical across all titles,
while the GB/GBC path is designed around a locked 60 fps presentation.

Benchmarks in the development logs are measurements from specific ROMs,
specific scenes, and specific firmware revisions. They are therefore useful
for engineering comparison but should not be interpreted as a universal
per-game guarantee.

---

# Screenshots

Current screenshots live in:

```text
docs/screenshots/
```

Build photos live in:

```text
docs/photos/
```

---

# Credits

This project stands on the work of the upstream projects below:

- **Charlanth/esp32-gba** â€” original ESP32-S3 GBA port and the starting point for
  this repository:
  https://github.com/Charlanth/esp32-gba
- **44vba** â€” GBA emulator/ESP32 work that this project grew from:
  https://github.com/44670/44vba
- **VBA-next** â€” upstream GBA core lineage:
  https://github.com/libretro/vba-next
- **gnuboy** â€” Game Boy emulator:
  https://github.com/rofl0r/gnuboy
- **retro-go** â€” reference/vendor source used for the GB/GBC side:
  https://github.com/ducalex/retro-go
- **Freenove** â€” original board documentation relevant to the inherited hardware
  work.

---

# Status

This repository is intended to be a **standalone firmware project for the
current ESP32-S3 handheld**.

It is no longer a generic collection of all targets present in the original
repository. The code, configuration, documentation, and tools are deliberately
focused on one hardware configuration so that a fresh checkout has a clear
build target and a clear flashing procedure.

For current hardware wiring, use `WIRING.md`.

For historical engineering decisions, use:

```text
docs/devlog/FINDINGS.md
docs/devlog/SESSION.md
```
