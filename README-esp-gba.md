# esp-gba

[44vba](https://github.com/44670/44vba) (a `vba-next` fork) running on this ESP32-S3 board,
built with PlatformIO instead of a system ESP-IDF install.

Upstream files are untouched except for one added stub (`port-esp32s3/main/config.h`).
Everything else here is additive: `platformio.ini`, `sdkconfig.defaults`, `partitions.csv`,
`components/gba/`, `tools/`.

## The board

Read off the chip with `esptool.py flash_id`:

| | |
|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2 |
| PSRAM | 8 MB octal, AP Memory, 3V3 (`opi psram: vendor id 0x0d (AP)`, 64 Mbit) |
| Flash | 16 MB, quad per eFuse |
| USB bridge | CH340/CH343 (`1a86:*`), enumerates as `/dev/ttyACM0` |

It is **not** a genuine Espressif DevKitC-1 — that ships a CP2102N (`303a:1001`). Relevant
because PlatformIO's `esp32-s3-devkitc-1` board manifest makes assumptions that do not hold
here (see "Flash mode" below).

## Five things that had to be fixed

### 1. `config.h` is missing from upstream

`port-esp32s3/main/{main.cpp,os.c}` both `#include "config.h"`, but no such file exists in
the repo — the `.gitignore` swallows it. **`port-esp32s3` does not compile as published.**
Neither file references anything from it, so `port-esp32s3/main/config.h` is an empty stub.

### 2. Flash mode must be DIO, not QIO

This one costs an afternoon if you don't spot it. PlatformIO's `esp32-s3-devkitc-1` manifest
hardcodes `"flash_mode": "qio"`, and that is stamped into the image header — it **overrides**
`CONFIG_ESPTOOLPY_FLASHMODE_*` in sdkconfig, so changing sdkconfig alone does nothing.
(`esptool` even warns: *"Image file at 0x0 is protected with a hash checksum, so not changing
the flash mode setting."*)

With QIO this board's ROM loader misreads the bootloader image and boot-loops forever:

```
mode:QIO, clock div:1
load:0x3fce3808,len:0x162c     <-- one bogus segment; the image actually has 3
ets_loader.c 78
rst:0x10 (RTCWDT_RTC_RST)      <-- alternating with TG0WDT_SYS_RST
```

Note it never reaches `entry`, and no 2nd-stage bootloader banner is printed — which is how
you know it is *not* a PSRAM problem. PSRAM init happens much later.

The fix is in `platformio.ini`, not `sdkconfig.defaults`:

```ini
board_build.flash_mode = dio
```

A healthy boot loads three segments and prints `entry`.

### 3. `emuInit()` was missing the per-game overrides — black screen

Upstream's ESP32 `emuInit()` is only:

```c
CPUSetupBuffers(); CPUInit(NULL, false); CPUReset(); SetFrameskip(0x1);
```

`port-sdl2/main.cpp` — the port that actually works — additionally calls
`load_image_preferences()`, `soundReset()`, `rtcEnable(true)` and `flashSetSize()`, and seeds
`cpuSaveType`/`flashSize`/`enableRtc`/`mirroringEnable` first. `load_image_preferences()` lives
in `libretro/libretro.cpp`, which the ESP32 port does not compile, so **none of it ran**.

Pokémon Gen 3 probes its save chip and RTC during boot. Without those settings Ruby hangs
before initialising the display: the emulator runs at full speed and renders pure black
forever. The tell is `DISPCNT=0x0080` — bit 7 is forced blank — with `pix_nonzero=0`.
Once fixed, `DISPCNT` becomes `0x1f40` (BG0-3 + OBJ enabled) and pixels appear.

`gbaover[]` + `load_image_preferences()` are extracted verbatim into
`port-esp32s3/main/gbaover.cpp` (from `libretro.cpp` lines 226-424) rather than linking
`libretro.cpp`, which would drag in the whole libretro frontend API. For Ruby (Japan) the
table gives `flashSize=131072, saveType=0, rtcEnabled=1, mirroringEnabled=0`.

### 4. Partition table rebuilt for 16 MB

Upstream assumes an 8 MB-flash N8R8 (`2M app + 4M rom`). This board has 16 MB, so:

```
factory  app  0x010000   4M
rom      0x40 0x410000  11M
```

The ROM is not a filesystem — it is `esp_partition_mmap`'d straight into the address space
(`main.cpp`), so **the whole cart must fit in the `rom` partition**. Flash a ROM with:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/ttyACM0 \
  --baud 921600 write_flash 0x410000 "/path/to/game.gba"
```

### 5. PlatformIO needs `setuptools < 81`

`espressif32@5.4.0`'s IDF builder does `import pkg_resources`, which setuptools 81 removed.
On a Python 3.14 pipx venv the build dies before compiling anything:

```bash
pipx inject platformio "setuptools<81" --force
```

### Bonus gotcha: console baud rate

`CONFIG_ESP_CONSOLE_UART_BAUDRATE` in `sdkconfig.defaults` is silently ignored. IDF 4.4's
Kconfig declares it `prompt "UART console baud rate" if ESP_CONSOLE_UART_CUSTOM`, so without a
prompt Kconfig discards the value and forces 115200. You must also set
`CONFIG_ESP_CONSOLE_UART_CUSTOM=y` (which keeps UART0 on its normal S3 pins, TX 43 / RX 44).

Also note PlatformIO only regenerates `sdkconfig.esp32s3` when its cache is invalidated -- after
editing `sdkconfig.defaults`, `rm -f sdkconfig.esp32s3` before rebuilding or the change may not
take.

## Why ESP-IDF 4.4 (`espressif32@5.4.0`)

Upstream is written against IDF 4.4. IDF 5.x renamed `esp_partition_mmap`'s handle type and
the `SPI_FLASH_MMAP_DATA` enum, and moved per-target `CONFIG_ESP32S3_SPIRAM_SUPPORT` to
`CONFIG_SPIRAM`. Moving to 5.x means patching the source; the pin avoids that.

## ROM size limits

| ROM | Size | Fits in 11 MB `rom` partition? |
|---|---|---|
| Pocket Monsters - Ruby (Japan) `AXVJ` | 8.00 MB | yes |
| Pocket Monsters - Emerald (Japan) `BPEJ` | 16.00 MB | **no** |
| Pocket Monsters - FireRed (Japan) `BPRJ` | 16.00 MB | **no** |

The Japanese Gen 3 releases are not all the same size — Ruby is 8 MB while Emerald and
FireRed are full 16 MB carts. Trimming trailing `0xFF` padding does not rescue the 16 MB ones
(real data ends at 15.25 MB and 15.87 MB respectively).

**No partition layout can fit a 16 MB ROM on 16 MB of flash**: 16.00 − 0.06 (bootloader/nvs/phy)
− app leaves at most 13.94 MB with a 2 MB app, or 14.94 MB with a 1 MB app. Running those carts
requires replacing the `esp_partition_mmap` path with a demand-paged SD reader backed by a
PSRAM cache — which upstream does not implement.

## Optimisation

Benchmark: `tools/bench.py` resets the board and averages over a fixed window. From reset with
no input the Ruby attract mode is deterministic, so the same wall-clock window covers the same
workload on every build — scene complexity varies enormously (a fade is far cheaper than the
scrolling forest), so a free-running average is not comparable between runs.

**Measure emulated time from audio sample count, not from a frame callback.** The first version
of this benchmark counted `systemOnWriteDataToSoundBuffer` calls as frames; that callback fires
~1.7x per GBA frame, which inflated every absolute number. `audioSamples / 47872` is seconds of
emulated game time and is unambiguous. The giveaway was `emu/draw` sitting at a constant 1.7
regardless of frameskip, which is impossible if the callback were once per frame.

Results at frameskip 1, drawn fps (directly measured both before and after):

| Change | drawn fps | Δ |
|---|---|---|
| Baseline (upstream settings) | 13.0 | — |
| PSRAM 40 → 80 MHz | 13.4 | +3% |
| Assertions disabled | 13.4 | 0% |
| `-O2` → `-O3` | 13.1 | **−2%**, reverted |
| **Async DMA framebuffer blit** | 16.6 | **+24%** |
| `vram` → internal SRAM | — | no-op, allocation failed |
| QIO flash (app only) | 17.0 | +3% |
| 32-bit byteswap loop | 17.0 | 0% |
| 64-byte data cache line | 17.2 | +1% |
| **Total** | **17.0** | **+31%** |

What actually mattered was one thing: **the blocking SPI blit**. Upstream called
`spi_device_transmit()`, so the CPU sat idle ~10ms per frame waiting for 76800 bytes to clock
out at 60MHz. Queuing it and reclaiming the transfer at the top of the next frame recovers
essentially all of that, and still works with a real panel attached.

Everything aimed at memory bandwidth — faster PSRAM, QIO flash, bigger cache lines, moving hot
buffers to internal SRAM — bought a few percent each. The emulator is CPU-bound in the ARM
interpreter, not starved for bandwidth. `-O3` was actively worse: bigger code, more instruction
cache pressure.

Two things worth knowing for future attempts:

- **Internal SRAM is more fragmented than the totals suggest.** 160191 bytes free but the
  largest contiguous block is only 98304, so neither `pix` (163840) nor `vram` (131072) fits.
  `FB` alone is 76800 bytes of static internal. The code tries internal first and falls back.
- **QIO needs the bootloader left on DIO.** The ROM loader cannot read QIO on this board
  (the flash's Quad Enable bit is not set that early), but the IDF bootloader sets QE and can
  then read the app in QIO — `qio_mode: Enabling default flash chip QIO` in the boot log.
  `tools/flash_qio.sh` builds everything QIO and patches only the bootloader image back to DIO,
  recomputing its SHA256. **Use that script, not `pio run -t upload`**, which would flash a QIO
  bootloader and boot-loop.

### Frameskip

Runtime-tunable over the control channel (`tools/bench.py <label> <warmup> <window> <port> <n>`):

| frameskip | emulated fps | % of full speed |
|---|---|---|
| 0 | 12.6 | 21% |
| 1 (default) | 17.6 | 29% |
| 2 | 21.0 | 35% |
| 3 | 23.0 | 38% |

Note `emulated fps ≈ drawn fps` at every setting, so frameskip here reduces per-frame rendering
work rather than reducing how often the framebuffer is blitted. **This measures throughput, not
perceived smoothness** — higher settings repeat rendered content, and how bad that looks has not
been assessed. The default stays at 1, which is what the captured video shows.

## Measured performance

Pokémon Ruby (Japan), frameskip 1, PSRAM at 40 MHz, 240 MHz CPU:

| State | drawn fps |
|---|---|
| Forced-blank screen (broken, pre-fix) | 35 |
| Gameplay, upstream settings | 13.0 |
| Gameplay, optimised (frameskip 1) | **17.0** |
| Gameplay, optimised (frameskip 3) | 22.5 |

Upstream's README claims 20 fps on an N8R8 but does not say what was on screen — and as the
first row shows, a blank screen more than doubles the number, so the two figures may not be
comparable.

Measured with no panel attached. The SPI writes still execute and cost real time, so wiring a
display should not change this much, but it is not zero-risk.

Still short of the 59.72 fps the GBA runs at — roughly 29% of full speed at the default
frameskip. Closing that gap needs the ARM interpreter itself to get cheaper (IRAM placement of
the hot dispatch path, or a dynarec), not more memory bandwidth.

## Not done yet

- **No display.** `os.c` drives a 240x240 ST7789 on GPIO 10/11/12/13/14 + reset 21. The panel
  on order is an ILI9341 320x240, which needs a different init sequence and a new window/offset
  calculation. Nothing has been written for it.
- **No input.** `os.h` expects 8 buttons on GPIO 0/8/18/38/39/45/46/48. Careful: GPIO 0, 45 and
  46 are strapping pins on the S3 — holding those buttons at reset will change boot mode.
- **No audio *output*.** The core does generate sound — upstream just discarded it in an empty
  `systemOnWriteDataToSoundBuffer()`. That callback is now implemented and the samples are
  captured over UART (see Recording), which confirms the audio path works end to end: the rate
  derived from sample counts lands within 0.3% of the 47782 Hz passed to `soundSetSampleRate()`.
  What is missing is a hardware sink. The ESP32-S3 has **no DAC** (unlike the original ESP32),
  so playback needs an I2S amp such as a MAX98357A — see WIRING.md.
- **No save persistence.** `libretro_save_buf` is allocated but never written to flash or SD,
  so in-game saves are lost on reset.

## Bringing up an unfamiliar board (headless mode)

```bash
rm -rf .pio/build/esp32s3        # required: cmake only re-reads the env var on reconfigure
ESP_GBA_HEADLESS=1 ./tools/flash_qio.sh
```

`HEADLESS` skips `lcdInit()` and all GPIO setup, so the firmware drives **no pins at all**. Use
it on any board whose pinout you do not know: this port's ST7789 pins (GPIO 10/11/12/13/14/21)
will be wired to something else there, and clocking them can buzz an onboard amplifier or
contend with another chip's outputs.

It also skips `osSerialInit()`. That matters on boards whose USB is the S3's **native**
USB-Serial/JTAG rather than a UART bridge: `esp_vfs_dev_uart_use_driver()` redirects stdout to
physical UART0 on GPIO 43/44, which on such a board goes nowhere, and every subsequent print
vanishes. Leaving stdout on the default console keeps the log visible over USB.

**The serial control channel does not work in headless mode** — key injection and framebuffer
dumps are bound to `UART_NUM_0`. Screenshots and `record.py` need a UART-bridge board, or a port
of the channel to the `usb_serial_jtag` driver.

### Freenove FNK0104AB (2.8" ILI9341)

```bash
rm -rf .pio/build/esp32s3
ESP_GBA_BOARD=FNK0104AB ./tools/flash_qio.sh
```

Pin map from the vendor's own TFT_eSPI setup header
(`Libraries/FNK0104AB/TFT_eSPI_Setups_v1.3.zip` → `FNK0104AB_2.8_240x320_ILI9341.h`
in [Freenove/Freenove_ESP32_S3_Display](https://github.com/Freenove/Freenove_ESP32_S3_Display)),
not guessed:

| Signal | GPIO | Upstream ST7789 build used |
|---|---|---|
| SCLK | 12 | 12 |
| MOSI | 11 | 13 |
| MISO | 13 | 14 |
| CS | 10 | 11 |
| DC | 46 | 10 |
| RST | tied to board reset | 21 |
| Backlight | 45 (HIGH = on) | not driven |

Panel is 240x320 native, driven rotated to 320x240 landscape (MADCTL `0x28` = MV|BGR) with
`INVON`, per the vendor setup's `TFT_RGB_ORDER TFT_BGR` and `TFT_INVERSION_ON`. The 240x160 GBA
frame is centred, leaving a 40px border all round. SPI at 40 MHz, the vendor's figure.

**Pins this board uses that the generic build would have trampled:**

```
ES8311 codec  I2S : MCK 17, BCK 18, DIN 16, DOUT 15, WS 21, amp enable 1
ES8311 codec  I2C : SCL 39, SDA 38
SDMMC             : CLK 5, CMD 4, D0 6, D1 7, D2 2, D3 3
```

Upstream's `PIN_SYS_RSTN` was **21 — the codec's word-select line** — and `lcdInit()` drives it
low for 500 ms at every boot. The remapped button pins (4,5,6,7,15,16,17,18) land squarely on
the SD card and I2S lines. Hence `PIN_KEY_*` are all `-1` in the FNK0104AB block: this board has
no discrete buttons, and defining them would fight the onboard peripherals.

**The serial control channel does not work on this board.** Console *output* reaches USB via
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`, but input is read from `UART_NUM_0` on GPIO
43/44, which the native-USB port does not reach. Screenshots, key injection and `record.py` all
need a port to the `usb_serial_jtag` driver.

### Two display bugs that cost an evening

Both were self-inflicted, and both were invisible to every check that existed at the time —
the build passed, the benchmark improved, and the UART screenshots were pixel-perfect, because
**the framebuffer was always correct and only what reached the panel was wrong**. Neither board
had a working screen when the code was written.

**1. Transactions over 32768 bytes are silently truncated.** `SPI_LL_DATA_MAX_BIT_LEN` is
`1 << 18` bits on the ESP32-S3. `spi_master.c` only validates a transaction against
`max_transfer_sz`, never against the hardware limit, so a 76800-byte frame returns `ESP_OK` and
delivers 32768 bytes — the top 68 of 160 rows. Upstream's 28800-byte chunking was already under
the limit; collapsing it into one transfer for speed reintroduced the bug.

**2. CS must stay LOW across a command and its data.** This panel discards the operation if CS
rises between `0x2C` and the pixel data. Upstream raised CS after every transfer, which its
ST7789 tolerated; this ILI9341 does not. `lcdCsLow()`/`lcdCsHigh()` are nested so
`lcdSetWindow()` can be called inside a blit without releasing CS.

The give-away was that `lcdReadReg()` worked perfectly while everything else failed — it is the
one function that puts command and data in a *single* transaction.

### Verifying the panel without looking at it

Register reads on this board are trustworthy, which makes the display self-checking. Don't
assume — prove it first, by writing two different values and confirming the readback tracks:

```
read check: wrote 28 -> 28, wrote 48 -> 48  => reads TRUSTWORTHY
```

(An earlier version of this README quoted a `MADCTL` readback as evidence while `ID4` was
returning `00 00 00`. Run the check before believing any read.)

With that established, `lcdSelfTest()` writes 8 red pixels through the real blit path and reads
them back out of frame memory:

```
pixel probe: wrote RED x8, read back ffffff ...   <- broken (CS framing)
pixel probe: wrote RED x8, read back fc0000 ...   <- fixed  (fc vs f8 = 5->6 bit expansion)
```

`lcdProbeRow(x, y, n)` samples panel RAM while the game runs, which verifies the whole chain
end to end:

```
panel probe @(140,120): 0/8 non-black   DISPCNT=0140   <- game still booting
panel probe @(140,120): 8/8 non-black, first px 103800  DISPCNT=1f40   <- forest intro
```

Note `ID4` reads `00 00 00` on this board even though other reads are reliable — the controller
is likely a clone that does not implement `0xD3`. It cannot be used to identify the panel.

### Second optimisation round (on the panel)

| Attempt | Result |
|---|---|
| Async blit restored, CS held low across the frame | **14.6 → 18.8 fps (+29%)** |
| SPI 40 → 60 MHz | no change; kept (less bus time, verified on panel) |
| `vram` → internal SRAM, `FB` → PSRAM | **failed**, see below |
| `CONFIG_ESP32S3_*_CACHE_WRAP` | **does not link** on IDF 4.4 with octal PSRAM |

Only the first helped. Everything else was flat or impossible, and the remaining headroom is in
the ARM interpreter itself, not the display path.

**`spi_device_queue_trans()` with a PSRAM source hangs.** The transaction never completes and
`lcdWaitFB()` blocks forever on `spi_device_get_trans_result()`. So `FB` must be internal — and
since internal SRAM has only ~161KB contiguous at `app_main` entry, `vram` (131072) and `FB`
(76800) cannot both be internal. `FB` wins because the async blit depends on it.

Allocation order matters if you retry this: at `app_main` entry there is one ~161KB block, but
after `osInit()` (SPI DMA descriptors, UART driver) the largest is ~90KB.

**`CONFIG_ESP32S3_INSTRUCTION_CACHE_WRAP` / `DATA_CACHE_WRAP`** fail at link with
`undefined reference to psram_support_wrap_size`. Note this fails at *link*, so a stale binary
stays on the board and the next benchmark silently measures the old firmware — always confirm
the flash actually wrote (`Hash of data verified` x3) before trusting a number.

### Cost of the fix

The blit is blocking again (`spi_device_transmit`, manual CS), so the 24% async gain is
currently given back: ~14.6 fps here versus 18.8 with the broken async path and ~19 headless.
Re-optimising is now safe to attempt because `lcdProbeRow` can confirm each step actually
reaches the glass. The likely route is queued transfers with CS held low across the whole
frame, released in a post-transfer callback.

### Second board (verified)

| | Board A | Board B |
|---|---|---|
| USB | CH340/CH343 bridge (`1a86:*`) | native USB-Serial/JTAG (`303a:1001`) |
| Flash | 16 MB, vendor `0x46` | 16 MB, vendor `0x5e` |
| PSRAM | 8 MB octal (AP_3v3) | 8 MB octal (AP_3v3), eFuse `PSRAM_CAP=8M` |
| MAC | `ac:a7:04:fd:e9:78` | `44:1b:f6:ce:63:ac` |
| QIO app | works | works |
| Emulated fps | 17.2 (with blit) | ~19 (headless) |

Both are R8-class, so the same build runs on either. Board B has a built-in screen, speaker,
microphone and SD slot, none of which this firmware touches yet.

## Recording without a panel

There is no display or button hardware yet, so a serial control channel stands in for both.
`os.c` injects host-sent keys into `osReadKey()` (real GPIO buttons keep working once wired)
and streams the framebuffer and PCM back between `<FB n>`/`<AU n>` markers.

`OS_CMD_STEP` advances the emulator **exactly one frame per request** and idles in between.
This matters: a frame is 76800 bytes and takes ~0.5s on the wire, far longer than it takes to
emulate. If the emulator free-ran during transfers the recording would have gaps and the audio
would drift out of sync. Capture runs slower than real time (~2.7 frames/s) but is continuous
and frame-accurate. Audio is returned every step while video is optional per step, so video can
be recorded at a lower rate than the emulated frame rate without holes in the soundtrack.

```bash
python tools/play.py shots "wait 300, START:8, wait 60, shot title"   # free-running
python tools/record.py out.mp4 --frames 700 --video-every 2           # step-recorded A/V
```

Two host-side gotchas that cost real time:

- `pyserial`'s `read(n)` blocks for the **full timeout** whenever fewer than `n` bytes are
  pending. Using `read(in_waiting or 1)` doubled capture throughput; baud was not the limit.
- The console VFS translates `\n` to `\r\n`, so headers arrive as `<FB 76800>\r\n`. Match `>`
  and skip the EOL rather than looking for a fixed terminator.

## Usage

```bash
pio run -e esp32s3 -t upload                          # build + flash firmware
python tools/capture_boot.py 15                       # reset and watch the boot log
```

Note `tools/capture_boot.py` takes a baud argument; the console runs at 1500000 (the ROM
bootloader's own messages are always 115200 and will look like garbage at that rate).
