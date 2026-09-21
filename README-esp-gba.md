# esp-gba

[44vba](https://github.com/44670/44vba) (a `vba-next` fork) running on this ESP32-S3 board,
built with PlatformIO instead of a system ESP-IDF install.

**Just want to put it on the board? See [FLASHING.md](FLASHING.md)** â€”
step-by-step for Linux and Windows, including the SD card layout. The rest
of this file is the development story.

Upstream files are untouched except for one added stub (`components/esp_gba/config.h`).
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

It is **not** a genuine Espressif DevKitC-1 â€” that ships a CP2102N (`303a:1001`). Relevant
because PlatformIO's `esp32-s3-devkitc-1` board manifest makes assumptions that do not hold
here (see "Flash mode" below).

## Five things that had to be fixed

### 1. `config.h` is missing from upstream

`components/esp_gba/{main.cpp,os.c}` both `#include "config.h"`, but no such file exists in
the repo â€” the `.gitignore` swallows it. **the original port did not compile as published without restoring its ignored config.h.**
Neither file references anything from it, so `components/esp_gba/config.h` is an empty stub.

### 2. Flash mode must be DIO, not QIO

This one costs an afternoon if you don't spot it. PlatformIO's `esp32-s3-devkitc-1` manifest
hardcodes `"flash_mode": "qio"`, and that is stamped into the image header â€” it **overrides**
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

Note it never reaches `entry`, and no 2nd-stage bootloader banner is printed â€” which is how
you know it is *not* a PSRAM problem. PSRAM init happens much later.

The fix is in `platformio.ini`, not `sdkconfig.defaults`:

```ini
board_build.flash_mode = dio
```

A healthy boot loads three segments and prints `entry`.

### 3. `emuInit()` was missing the per-game overrides â€” black screen

Upstream's ESP32 `emuInit()` is only:

```c
CPUSetupBuffers(); CPUInit(NULL, false); CPUReset(); SetFrameskip(0x1);
```

The ESP32-S3 emuInit() additionally calls
`load_image_preferences()`, `soundReset()`, `rtcEnable(true)` and `flashSetSize()`, and seeds
`cpuSaveType`/`flashSize`/`enableRtc`/`mirroringEnable` first. `load_image_preferences()` lives
in `libretro/libretro.cpp`, which the ESP32 port does not compile, so **none of it ran**.

PokÃ©mon Gen 3 probes its save chip and RTC during boot. Without those settings Ruby hangs
before initialising the display: the emulator runs at full speed and renders pure black
forever. The tell is `DISPCNT=0x0080` â€” bit 7 is forced blank â€” with `pix_nonzero=0`.
Once fixed, `DISPCNT` becomes `0x1f40` (BG0-3 + OBJ enabled) and pixels appear.

`gbaover[]` + `load_image_preferences()` are extracted verbatim into
`components/esp_gba/gbaover.cpp` (from `libretro.cpp` lines 226-424) rather than linking
`libretro.cpp`, which would drag in the whole libretro frontend API. For Ruby (Japan) the
table gives `flashSize=131072, saveType=0, rtcEnabled=1, mirroringEnabled=0`.

### 4. Partition table rebuilt for 16 MB

Upstream assumes an 8 MB-flash N8R8 (`2M app + 4M rom`). This board has 16 MB, so:

```
factory  app  0x010000   4M
rom      0x40 0x410000  11M
```

The ROM is not a filesystem â€” it is `esp_partition_mmap`'d straight into the address space
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

> **Superseded.** Sparse packing now aliases identical padding pages (all
> 0xFF and all 0x00) into shared slots, so what must fit is a cart's
> *distinct* pages â€” every 16 MB cart tested except Fire Emblem (US) fits.
> The section below records the earlier flat-mapping era.

| ROM | Size | Fits in 11 MB `rom` partition? |
|---|---|---|
| Pocket Monsters - Ruby (Japan) `AXVJ` | 8.00 MB | yes |
| Pocket Monsters - Emerald (Japan) `BPEJ` | 16.00 MB | **no** |
| Pocket Monsters - FireRed (Japan) `BPRJ` | 16.00 MB | **no** |

The Japanese Gen 3 releases are not all the same size â€” Ruby is 8 MB while Emerald and
FireRed are full 16 MB carts. Trimming trailing `0xFF` padding does not rescue the 16 MB ones
(real data ends at 15.25 MB and 15.87 MB respectively).

**No partition layout can fit a 16 MB ROM on 16 MB of flash**: 16.00 âˆ’ 0.06 (bootloader/nvs/phy)
âˆ’ app leaves at most 13.94 MB with a 2 MB app, or 14.94 MB with a 1 MB app. Running those carts
requires replacing the `esp_partition_mmap` path with a demand-paged SD reader backed by a
PSRAM cache â€” which upstream does not implement.

## Optimisation

Benchmark: `tools/bench.py` resets the board and averages over a fixed window. From reset with
no input the Ruby attract mode is deterministic, so the same wall-clock window covers the same
workload on every build â€” scene complexity varies enormously (a fade is far cheaper than the
scrolling forest), so a free-running average is not comparable between runs.

**Measure emulated time from audio sample count, not from a frame callback.** The first version
of this benchmark counted `systemOnWriteDataToSoundBuffer` calls as frames; that callback fires
~1.7x per GBA frame, which inflated every absolute number. `audioSamples / 47872` is seconds of
emulated game time and is unambiguous. The giveaway was `emu/draw` sitting at a constant 1.7
regardless of frameskip, which is impossible if the callback were once per frame.

Results at frameskip 1, drawn fps (directly measured both before and after):

| Change | drawn fps | Î” |
|---|---|---|
| Baseline (upstream settings) | 13.0 | â€” |
| PSRAM 40 â†’ 80 MHz | 13.4 | +3% |
| Assertions disabled | 13.4 | 0% |
| `-O2` â†’ `-O3` | 13.1 | **âˆ’2%**, reverted |
| **Async DMA framebuffer blit** | 16.6 | **+24%** |
| `vram` â†’ internal SRAM | â€” | no-op, allocation failed |
| QIO flash (app only) | 17.0 | +3% |
| 32-bit byteswap loop | 17.0 | 0% |
| 64-byte data cache line | 17.2 | +1% |
| **Total** | **17.0** | **+31%** |

What actually mattered was one thing: **the blocking SPI blit**. Upstream called
`spi_device_transmit()`, so the CPU sat idle ~10ms per frame waiting for 76800 bytes to clock
out at 60MHz. Queuing it and reclaiming the transfer at the top of the next frame recovers
essentially all of that, and still works with a real panel attached.

Everything aimed at memory bandwidth â€” faster PSRAM, QIO flash, bigger cache lines, moving hot
buffers to internal SRAM â€” bought a few percent each. The emulator is CPU-bound in the ARM
interpreter, not starved for bandwidth. `-O3` was actively worse: bigger code, more instruction
cache pressure.

Two things worth knowing for future attempts:

- **Internal SRAM is more fragmented than the totals suggest.** 160191 bytes free but the
  largest contiguous block is only 98304, so neither `pix` (163840) nor `vram` (131072) fits.
  `FB` alone is 76800 bytes of static internal. The code tries internal first and falls back.
- **QIO needs the bootloader left on DIO.** The ROM loader cannot read QIO on this board
  (the flash's Quad Enable bit is not set that early), but the IDF bootloader sets QE and can
  then read the app in QIO â€” `qio_mode: Enabling default flash chip QIO` in the boot log.
  `tools/flash_qio.py` builds everything QIO and patches only the bootloader image back to DIO,
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

Note `emulated fps â‰ˆ drawn fps` at every setting, so frameskip here reduces per-frame rendering
work rather than reducing how often the framebuffer is blitted. **This measures throughput, not
perceived smoothness** â€” higher settings repeat rendered content, and how bad that looks has not
been assessed. The default stays at 1, which is what the captured video shows.

## Measured performance

PokÃ©mon Ruby (Japan), frameskip 1, PSRAM at 40 MHz, 240 MHz CPU:

| State | drawn fps |
|---|---|
| Forced-blank screen (broken, pre-fix) | 35 |
| Gameplay, upstream settings | 13.0 |
| Gameplay, optimised (frameskip 1) | **17.0** |
| Gameplay, optimised (frameskip 3) | 22.5 |

Upstream's README claims 20 fps on an N8R8 but does not say what was on screen â€” and as the
first row shows, a blank screen more than doubles the number, so the two figures may not be
comparable.

Measured with no panel attached. The SPI writes still execute and cost real time, so wiring a
display should not change this much, but it is not zero-risk.

Still short of the 59.72 fps the GBA runs at â€” roughly 29% of full speed at the default
frameskip. Closing that gap needs the ARM interpreter itself to get cheaper (IRAM placement of
the hot dispatch path, or a dynarec), not more memory bandwidth.

## Current hardware

This port targets one fixed ESP32-S3 handheld configuration.

### Display

The handheld uses a **320Ã—240 ILI9341 SPI display**.

The display is driven from the same SPI bus used by the SD card:

| Signal | GPIO |
|---|---:|
| LCD CS | 10 |
| LCD DC | 11 |
| SPI SCK | 12 |
| SPI MOSI | 13 |
| SPI MISO | 9 |
| Backlight | 14 |

LCD readback is disabled in the normal firmware path. The panel reset is tied
to the ESP32-S3 reset line.

The GBA framebuffer is 240Ã—160 and is centered on the 320Ã—240 LCD.

### Buttons

The physical button layout is:

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

Buttons are active-low and use the ESP32-S3 internal pull-ups.

### SD card

The SD card shares SPI clock, MOSI and MISO with the LCD.

| Signal | GPIO |
|---|---:|
| SD CS | 18 |
| SPI SCK | 12 |
| SPI MOSI | 13 |
| SPI MISO | 9 |

### Audio

Physical audio output is disabled in this handheld firmware.

The emulator sound-processing code remains present because it is part of the
emulation/timing path, but no physical audio output device is initialized.

### Battery

This hardware configuration has no battery-voltage ADC.

`osBatteryMv()` therefore returns `-1`; the firmware does not report battery
voltage.

### Saves

Game saves and emulator save states are supported by the current firmware.

See `WIRING.md` for the complete hardware reference.

## Headless build

A headless build is available for emulator/core testing without touching the
display or GPIO hardware.

```bash
rm -rf .pio/build/esp32s3
ESP_GBA_HEADLESS=1 pio run
```

## Display bring-up notes

Both were self-inflicted, and both were invisible to every check that existed at the time â€”
the build passed, the benchmark improved, and the UART screenshots were pixel-perfect, because
**the framebuffer was always correct and only what reached the panel was wrong**. Neither board
had a working screen when the code was written.

**1. Transactions over 32768 bytes are silently truncated.** `SPI_LL_DATA_MAX_BIT_LEN` is
`1 << 18` bits on the ESP32-S3. `spi_master.c` only validates a transaction against
`max_transfer_sz`, never against the hardware limit, so a 76800-byte frame returns `ESP_OK` and
delivers 32768 bytes â€” the top 68 of 160 rows. Upstream's 28800-byte chunking was already under
the limit; collapsing it into one transfer for speed reintroduced the bug.

**2. CS must stay LOW across a command and its data.** This panel discards the operation if CS
rises between `0x2C` and the pixel data. Upstream raised CS after every transfer, which its
ST7789 tolerated; this ILI9341 does not. `lcdCsLow()`/`lcdCsHigh()` are nested so
`lcdSetWindow()` can be called inside a blit without releasing CS.

The give-away was that `lcdReadReg()` worked perfectly while everything else failed â€” it is the
one function that puts command and data in a *single* transaction.

### Verifying the panel without looking at it

Register reads on this board are trustworthy, which makes the display self-checking. Don't
assume â€” prove it first, by writing two different values and confirming the readback tracks:

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

Note `ID4` reads `00 00 00` on this board even though other reads are reliable â€” the controller
is likely a clone that does not implement `0xD3`. It cannot be used to identify the panel.

### Second optimisation round (on the panel)

| Attempt | Result |
|---|---|
| Async blit restored, CS held low across the frame | **14.6 â†’ 18.8 fps (+29%)** |
| SPI 40 â†’ 60 MHz | no change; kept (less bus time, verified on panel) |
| `vram` â†’ internal SRAM, `FB` â†’ PSRAM | **failed**, see below |
| `CONFIG_ESP32S3_*_CACHE_WRAP` | **does not link** on IDF 4.4 with octal PSRAM |

Only the first helped. Everything else was flat or impossible, and the remaining headroom is in
the ARM interpreter itself, not the display path.

**`spi_device_queue_trans()` with a PSRAM source hangs.** The transaction never completes and
`lcdWaitFB()` blocks forever on `spi_device_get_trans_result()`. So `FB` must be internal â€” and
since internal SRAM has only ~161KB contiguous at `app_main` entry, `vram` (131072) and `FB`
(76800) cannot both be internal. `FB` wins because the async blit depends on it.

Allocation order matters if you retry this: at `app_main` entry there is one ~161KB block, but
after `osInit()` (SPI DMA descriptors, UART driver) the largest is ~90KB.

**`CONFIG_ESP32S3_INSTRUCTION_CACHE_WRAP` / `DATA_CACHE_WRAP`** fail at link with
`undefined reference to psram_support_wrap_size`. Note this fails at *link*, so a stale binary
stays on the board and the next benchmark silently measures the old firmware â€” always confirm
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
