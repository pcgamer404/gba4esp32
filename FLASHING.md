# Flashing esp-gba (Linux & Windows)

Target hardware: **Freenove FNK0104** ("ESP32-S3 2.8 inch display board") —
ESP32-S3 N16R8, 320x240 ILI9341, ES8311 audio, microSD. Connect it over its
USB-C port; it shows up as a USB serial device with no extra drivers on
Linux and on Windows 10/11 (Espressif USB JTAG/serial, `303a:1001`).

## 1. Install PlatformIO Core

Python 3.9+ required.

**Linux:**

```bash
pipx install platformio
pipx inject platformio "setuptools<81"
```

**Windows (PowerShell):**

```powershell
py -m pip install --user platformio "setuptools<81"
```

The `setuptools<81` pin is not optional: the ESP-IDF builder this project
pins still imports `pkg_resources`, which newer setuptools removed. Without
the pin the build dies before compiling anything.

`pio` must be on PATH afterwards (`pio --version` to check; on Windows,
`py -m platformio --version` also works — substitute that spelling in the
command below if needed).

## 2. Get the code

```bash
git clone https://github.com/Charlanth/esp32-gba.git
cd esp32-gba
```

## 3. Build + flash

```bash
python tools/flash_qio.py
```

That single command builds the firmware and flashes it. The serial port is
auto-detected; if you have several serial devices, name it:

```bash
python tools/flash_qio.py --port /dev/ttyACM0    # Linux
python tools/flash_qio.py --port COM5            # Windows
```

First build downloads the toolchain and ESP-IDF (~1GB); later builds take
seconds. The old `tools/flash_qio.sh` does the same thing, Linux-only.

> **Never flash with `pio run -t upload`.** This board's ROM loader cannot
> read a QIO bootloader, and that is exactly what the default upload flashes
> — the board boot-loops until rescued by `flash_qio.py`. The script flashes
> a DIO-patched bootloader with the QIO app, which is the combination this
> hardware needs.

Linux permissions: if flashing fails with "permission denied" on the port,
add yourself to the serial group and re-login
(`sudo usermod -aG uucp $USER` on Arch, `dialout` on Debian/Ubuntu).

## 4. Prepare the SD card

FAT32-formatted card, with these folders at the root:

```
/roms      <- .gba / .gb / .gbc files go here
/saves     <- created automatically; back this up, it holds your saves
/packed    <- optional pack caches (see below)
/art       <- optional 48x48 box art (tools/scrape_art.py)
```

Games flash into the console's internal memory on first pick (a 16MB cart
takes ~2-4 minutes, once per game switch; picking the game already in
memory starts instantly). Pre-packing on the PC skips the slowest part of
that first pick:

```bash
python tools/pack_rom.py "/path/to/roms/Some Game.gba"
```

then copy the generated `packed/` files into the card's `/packed`.

Size limit: the console fits any cart whose *distinct* 64KB pages number
229 or fewer — that covers every popular cart tested (including 16MB ones
like Pokémon Emerald and Minish Cap, whose padding collapses into shared
pages). The rare cart that is 16MB of pure unique content (Fire Emblem US)
is refused with an on-screen message.

## 5. First boot

The console boots into a game picker (touch, or the BOOT button: short
press = next, long press = start). Pick a game; controls on the wired
button matrix are documented in `docs/button_wiring.svg`, with
Select+Left/Right acting as L/R and Select+Up toggling the debug overlay.
