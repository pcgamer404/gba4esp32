# esp-gba — a GBA / GB / GBC handheld on an ESP32-S3

A pocket emulator console built on the Freenove FNK0104 board (ESP32-S3, 2.8"
ILI9341 320x240, ES8311 audio codec, SD slot, LiPo charging). It plays Game
Boy Advance titles at ~50 fps with sound, and Game Boy / Game Boy Color
titles at a locked 60 fps.

Forked from [44vba](https://github.com/44670/44vba) (itself a
[vba-next](https://github.com/libretro/vba-next) fork); GB/GBC support is
[gnuboy](https://github.com/rofl0r/gnuboy) as vendored by
[retro-go](https://github.com/ducalex/retro-go).

## Features

- **GBA** via vba-next with a threaded scanline renderer, a native (HLE)
  m4a audio mixer, and per-game idle-loop skip — Pokémon gen-3 runs ~50 fps
  with audio at stock clocks.
- **GB / GBC** via gnuboy at a locked 60 fps, scaled 1.5x to 240x216.
- **Sound** through the board's ES8311 codec + speaker, rate-matched to the
  emulator's real speed so audio never crackles or drifts.
- **Game library menu** with box art from the SD card, button navigation,
  battery/charge readout, and a settings screen.
- **Saves**: GBA save flash persists to `/sd/saves/<game>.sav`; GB cart SRAM
  autosaves to `/sd/saves/<game>.srm`.
- **Charging indicator**: `CHG` in the menu footer (and a `+` next to the
  in-game battery gauge) whenever USB power is present.
- **Physical controls only** in-game: a 2x4 button matrix covers the full
  GBA pad, including L/R via Select-combos.

## Hardware

| Part | Detail |
|---|---|
| Board | Freenove FNK0104 (ESP32-S3, 16 MB quad flash, 8 MB octal PSRAM) |
| Screen | 2.8" ILI9341, 320x240, SPI at 60 MHz |
| Audio | ES8311 codec + SC8002B amp, I2S |
| Storage | micro-SD (SDMMC 4-bit) for ROMs, art, and saves |
| Power | LiPo + on-board TP4054 charger (~100 mA) |
| Buttons | 2x4 matrix on free GPIOs — rows 2/3, columns 14/21/43/44 |

Wiring: see [WIRING.md](WIRING.md) and the diagram in
[docs/button_wiring.svg](docs/button_wiring.svg). Each button simply bridges
its row pin to its column pin; rows are open-drain, columns use internal
pull-ups, no external resistors needed.

## Controls

| Input | Action |
|---|---|
| D-pad / A / B / Start / Select | GBA pad |
| Select + Left / Right | L / R triggers |
| Select + Up | toggle the debug/fps overlay |
| Menu: D-pad | move selection |
| Menu: A or Start | play |
| Menu: Select | settings (volume/mute, debug overlay, overclock, native audio) |

Nothing auto-starts: the console always boots to the picker and waits.

## SD card layout

```
/roms/     game files: .gba, .gb, .gbc
/art/      optional box art: <rom name without extension>.raw
           (48x48 RGB565 big-endian; tools/scrape_art.py fetches these)
/saves/    created automatically
/packed/   pack cache, created automatically (safe to delete)
```

## Build & flash

Requirements: [PlatformIO](https://platformio.org). The ESP-IDF build needs
`setuptools<81` in PlatformIO's Python env
(`~/.platformio/penv/bin/pip install "setuptools<81"`).

```bash
ESP_GBA_BOARD=FNK0104AB PORT=/dev/ttyACM0 bash tools/flash_qio.sh
```

Two things the script handles that a plain `pio run -t upload` gets wrong:

- `ESP_GBA_BOARD=FNK0104AB` selects this board's pin map. Without it you get
  the generic devkit build (wrong pins, and the link fails on the LCD code).
- The bootloader must be flashed DIO while the app runs QIO; the script
  patches the bootloader image accordingly (details in
  [README-esp-gba.md](README-esp-gba.md)).

For a factory-fresh state (default settings), also erase NVS once:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_region 0x9000 0x6000
```

## Performance notes

The native audio mixer (default on) plus a 260 MHz overclock (default on,
engages ~10 s into gameplay) runs gen-3 Pokémon at ~55 fps. The overclock
overdrives the shared PLL, which also pushes the flash clock ~8% out of
spec — safe for reads, but not for writes — so the firmware automatically
drops to stock around every flash/NVS write (game copies, settings saves)
and re-engages afterwards. It can be turned off in the settings screen;
all settings persist in NVS across power cycles.

A serial control channel (1.5 Mbaud on the USB port, `0xA5`-framed commands
in `port-esp32s3/main/os.h`) supports scripted picks, benchmarks, screenshots,
file push, and live tuning — everything `tools/*.py` uses.

## Repository layout

```
src/, libretro*/      vba-next core (GBA)
components/gnuboy/    gnuboy core (GB/GBC)
port-esp32s3/         this port: display, audio, SD, menu, input, serial
tools/                flash script, benchmark/screenshot/art tooling
docs/                 wiring diagram, dev logs (docs/devlog/)
port-sdl2/, port-wasm/, 44gba-watchos/   other 44vba ports, untouched
```

## Credits

- [44670/44vba](https://github.com/44670/44vba) — the ESP32 port this grew from
- [libretro/vba-next](https://github.com/libretro/vba-next) — the GBA core
- [ducalex/retro-go](https://github.com/ducalex/retro-go) — the gnuboy fork used for GB/GBC
- Freenove — FNK0104 board documentation

Development history, measurements, and the reasoning behind every port
decision live in [docs/devlog/](docs/devlog/).
